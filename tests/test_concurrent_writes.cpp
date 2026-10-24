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
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

using namespace klyrdb;
using namespace klyrdb::testing;
using namespace std::chrono_literals;

static void test_concurrent_serialized_inserts() {
    auto path = temp_test_file("test_concurrent_writes");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");

    auto pf = PageFile::create(path, false).value();
    BufferPoolManager bpm(*pf, 32);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();

    SqlExecutor executor(bpm, rm, *tc, *ic);
    EXPECT_TRUE(executor.execute("CREATE TABLE orders (id INTEGER PRIMARY KEY, item TEXT, amount INTEGER);").is_ok());
    EXPECT_TRUE(executor.execute("CREATE INDEX idx_amount ON orders(amount);").is_ok());

    constexpr size_t num_writers = 4;
    constexpr int rows_per_writer = 25;
    std::atomic<size_t> successful_inserts{0};
    std::atomic<size_t> failed_inserts{0};
    std::vector<std::thread> writers;

    for (size_t t = 0; t < num_writers; ++t) {
        writers.emplace_back([&, t]() {
            int start_id = static_cast<int>(t * rows_per_writer + 1);
            int end_id = start_id + rows_per_writer;
            for (int i = start_id; i < end_id; ++i) {
                std::string sql = "INSERT INTO orders VALUES (" + std::to_string(i) +
                                  ", 'item_" + std::to_string(i) + "', " + std::to_string(i * 5) + ");";
                auto res = executor.execute(sql);
                if (res.is_ok()) {
                    successful_inserts.fetch_add(1);
                } else {
                    failed_inserts.fetch_add(1);
                }
            }
        });
    }

    for (auto& w : writers) {
        w.join();
    }

    EXPECT_EQ(failed_inserts.load(), 0ULL);
    EXPECT_EQ(successful_inserts.load(), num_writers * rows_per_writer);

    // Verify row count
    auto count_res = executor.execute("SELECT id FROM orders;");
    EXPECT_TRUE(count_res.is_ok());
    EXPECT_EQ(count_res.value().row_count(), num_writers * rows_per_writer);

    // Verify each ID can be fetched via index seek
    for (int i = 1; i <= static_cast<int>(num_writers * rows_per_writer); ++i) {
        auto get_res = executor.execute("SELECT id, amount FROM orders WHERE id = " + std::to_string(i) + ";");
        EXPECT_TRUE(get_res.is_ok());
        EXPECT_EQ(get_res.value().row_count(), 1ULL);
        EXPECT_EQ(get_res.value().rows()[0].get(1).as_integer(), static_cast<int64_t>(i * 5));
    }
}

static void test_concurrent_primary_key_collision() {
    auto path = temp_test_file("test_pk_collision");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");

    auto pf = PageFile::create(path, false).value();
    BufferPoolManager bpm(*pf, 32);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();

    SqlExecutor executor(bpm, rm, *tc, *ic);
    EXPECT_TRUE(executor.execute("CREATE TABLE accounts (id INTEGER PRIMARY KEY, balance INTEGER);").is_ok());

    constexpr size_t num_threads = 4;
    std::atomic<size_t> success_count{0};
    std::atomic<size_t> duplicate_pk_errors{0};
    std::vector<std::thread> threads;

    // All threads race to insert the exact same primary key: id = 99
    for (size_t t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            std::string sql = "INSERT INTO accounts VALUES (99, " + std::to_string(t * 100) + ");";
            auto res = executor.execute(sql);
            if (res.is_ok()) {
                success_count.fetch_add(1);
            } else if (res.status().code() == StatusCode::ConstraintViolation) {
                duplicate_pk_errors.fetch_add(1);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // Exactly one insert must succeed; all others must fail with ConstraintViolation
    EXPECT_EQ(success_count.load(), 1ULL);
    EXPECT_EQ(duplicate_pk_errors.load(), num_threads - 1);

    auto sel = executor.execute("SELECT id FROM accounts;");
    EXPECT_TRUE(sel.is_ok());
    EXPECT_EQ(sel.value().row_count(), 1ULL);
}

int main() {
    std::cout << "=== Running Concurrent Writes Tests ===\n";
    run_test("test_concurrent_serialized_inserts", test_concurrent_serialized_inserts);
    run_test("test_concurrent_primary_key_collision", test_concurrent_primary_key_collision);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
