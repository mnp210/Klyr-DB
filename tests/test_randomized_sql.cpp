#include "test_framework.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/sql/executor.hpp"
#include <random>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

using namespace klyrdb;
using namespace klyrdb::testing;

struct UserModel {
    int64_t id;
    int64_t score;
    std::string tag;
};

static void test_randomized_crud_against_reference_model() {
    auto path = temp_test_file("test_random_crud");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);

    auto pf = PageFile::create(path, false).value();
    auto wal = WriteAheadLog::open_or_create(wal_path).value();
    BufferPoolManager bpm(*pf, 64);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();
    TransactionManager tm(bpm, *wal);

    SqlExecutor executor(bpm, rm, *tc, *ic);
    executor.set_transaction_manager(&tm);

    EXPECT_TRUE(executor.execute("CREATE TABLE accounts (id INTEGER PRIMARY KEY, score INTEGER, tag TEXT);").is_ok());
    EXPECT_TRUE(executor.execute("CREATE INDEX idx_score ON accounts(score);").is_ok());

    // Reference model
    std::map<int64_t, UserModel> model;

    std::mt19937_64 rng(1337);
    std::uniform_int_distribution<int> op_dist(0, 3); // 0: Insert, 1: Update, 2: Delete, 3: Query
    std::uniform_int_distribution<int64_t> id_dist(1, 100);
    std::uniform_int_distribution<int64_t> score_dist(10, 1000);

    const std::vector<std::string> tags = {"alpha", "beta", "gamma", "delta", "omega"};

    constexpr size_t NUM_OPERATIONS = 300;

    for (size_t op_i = 0; op_i < NUM_OPERATIONS; ++op_i) {
        int op = op_dist(rng);

        if (op == 0) {
            // INSERT
            int64_t id = id_dist(rng);
            int64_t score = score_dist(rng);
            std::string tag = tags[rng() % tags.size()];

            std::string sql = "INSERT INTO accounts VALUES (" + std::to_string(id) + ", " +
                              std::to_string(score) + ", '" + tag + "');";
            auto res = executor.execute(sql);

            if (model.find(id) == model.end()) {
                EXPECT_TRUE(res.is_ok());
                model[id] = UserModel{id, score, tag};
            } else {
                // Duplicate primary key must fail!
                EXPECT_TRUE(res.is_err());
            }
        } else if (op == 1 && !model.empty()) {
            // UPDATE
            // Pick an existing key or random key
            int64_t id = (rng() % 2 == 0) ? model.begin()->first : id_dist(rng);
            int64_t new_score = score_dist(rng);

            std::string sql = "UPDATE accounts SET score = " + std::to_string(new_score) +
                              " WHERE id = " + std::to_string(id) + ";";
            auto res = executor.execute(sql);
            EXPECT_TRUE(res.is_ok());

            if (model.find(id) != model.end()) {
                model[id].score = new_score;
                EXPECT_EQ(res.value().rows_affected(), 1ULL);
            } else {
                EXPECT_EQ(res.value().rows_affected(), 0ULL);
            }
        } else if (op == 2 && !model.empty()) {
            // DELETE
            int64_t id = (rng() % 2 == 0) ? model.begin()->first : id_dist(rng);
            std::string sql = "DELETE FROM accounts WHERE id = " + std::to_string(id) + ";";
            auto res = executor.execute(sql);
            EXPECT_TRUE(res.is_ok());

            if (model.find(id) != model.end()) {
                model.erase(id);
                EXPECT_EQ(res.value().rows_affected(), 1ULL);
            } else {
                EXPECT_EQ(res.value().rows_affected(), 0ULL);
            }
        } else {
            // QUERY
            if (rng() % 2 == 0 && !model.empty()) {
                // Exact point lookup
                auto it = model.begin();
                std::advance(it, rng() % model.size());
                int64_t target_id = it->first;

                std::string sql = "SELECT score, tag FROM accounts WHERE id = " + std::to_string(target_id) + ";";
                auto res = executor.execute(sql);
                EXPECT_TRUE(res.is_ok());
                EXPECT_EQ(res.value().rows().size(), 1ULL);
                EXPECT_EQ(res.value().rows()[0].get(0).as_integer(), it->second.score);
                EXPECT_EQ(res.value().rows()[0].get(1).as_text(), it->second.tag);
            } else {
                // Full table scan verification of total count
                auto res = executor.execute("SELECT * FROM accounts;");
                EXPECT_TRUE(res.is_ok());
                EXPECT_EQ(res.value().rows().size(), model.size());
            }
        }
    }

    // Final verification against reference model
    auto final_res = executor.execute("SELECT id, score, tag FROM accounts ORDER BY id ASC;");
    EXPECT_TRUE(final_res.is_ok());
    const auto& rows = final_res.value().rows();
    EXPECT_EQ(rows.size(), model.size());

    size_t idx = 0;
    for (const auto& [id, user] : model) {
        EXPECT_EQ(rows[idx].get(0).as_integer(), user.id);
        EXPECT_EQ(rows[idx].get(1).as_integer(), user.score);
        EXPECT_EQ(rows[idx].get(2).as_text(), user.tag);
        idx++;
    }
}

static void test_randomized_transaction_rollback_property() {
    auto path = temp_test_file("test_random_rollback");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);

    auto pf = PageFile::create(path, false).value();
    auto wal = WriteAheadLog::open_or_create(wal_path).value();
    BufferPoolManager bpm(*pf, 64);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();
    TransactionManager tm(bpm, *wal);

    SqlExecutor executor(bpm, rm, *tc, *ic);
    executor.set_transaction_manager(&tm);

    EXPECT_TRUE(executor.execute("CREATE TABLE data (k INTEGER PRIMARY KEY, v INTEGER);").is_ok());
    for (int i = 1; i <= 20; ++i) {
        EXPECT_TRUE(executor.execute("INSERT INTO data VALUES (" + std::to_string(i) + ", " + std::to_string(i * 10) + ");").is_ok());
    }

    std::mt19937_64 rng(999);

    for (int round = 0; round < 10; ++round) {
        // Begin transaction
        EXPECT_TRUE(executor.execute("BEGIN;").is_ok());

        // Perform random mutations
        for (int m = 0; m < 5; ++m) {
            int k = 1 + (rng() % 20);
            int v = 5000 + static_cast<int>(rng() % 1000);
            EXPECT_TRUE(executor.execute("UPDATE data SET v = " + std::to_string(v) + " WHERE k = " + std::to_string(k) + ";").is_ok());
        }

        // ROLLBACK
        EXPECT_TRUE(executor.execute("ROLLBACK;").is_ok());

        // Verify state is perfectly preserved
        auto q = executor.execute("SELECT k, v FROM data ORDER BY k ASC;");
        EXPECT_TRUE(q.is_ok());
        const auto& rows = q.value().rows();
        EXPECT_EQ(rows.size(), 20ULL);
        for (int i = 0; i < 20; ++i) {
            EXPECT_EQ(rows[i].get(0).as_integer(), i + 1);
            EXPECT_EQ(rows[i].get(1).as_integer(), (i + 1) * 10);
        }
    }
}

int main() {
    std::cout << "=== Running Property-Based & Randomized SQL Tests ===\n";

    run_test("test_randomized_crud_against_reference_model", test_randomized_crud_against_reference_model);
    run_test("test_randomized_transaction_rollback_property", test_randomized_transaction_rollback_property);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
