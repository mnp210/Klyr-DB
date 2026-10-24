#include "test_framework.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/sql/executor.hpp"
#include "klyrdb/sql/parser.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_unselective_cost_comparison() {
    auto path = temp_test_file("test_cost_comparison");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 64);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();

    SqlExecutor executor(bpm, rm, *tc, *ic);
    EXPECT_TRUE(executor.execute("CREATE TABLE data (id INTEGER PRIMARY KEY, val INTEGER);").is_ok());
    EXPECT_TRUE(executor.execute("CREATE INDEX idx_val ON data(val);").is_ok());

    // Insert 50 records
    for (int i = 1; i <= 50; ++i) {
        std::string ins = "INSERT INTO data VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ");";
        EXPECT_TRUE(executor.execute(ins).is_ok());
    }

    // 1. Highly selective query: val = 25 -> Plan should pick IndexSeek
    {
        auto expl_res = executor.execute("EXPLAIN SELECT * FROM data WHERE val = 25;");
        EXPECT_TRUE(expl_res.is_ok());
        std::string plan_text = expl_res.value().rows()[0].get(0).as_text();
        EXPECT_TRUE(plan_text.find("Index Seek") != std::string::npos);
    }

    // 2. Full scan: query with no predicate -> Table Scan
    {
        auto expl_res = executor.execute("EXPLAIN SELECT * FROM data;");
        EXPECT_TRUE(expl_res.is_ok());
        std::string plan_text = expl_res.value().rows()[0].get(0).as_text();
        EXPECT_TRUE(plan_text.find("Table Scan") != std::string::npos);
    }
}

static void test_optimization_with_transactions() {
    auto path = temp_test_file("test_opt_txn");
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile cleanup1(path);
    ScopedTempFile cleanup2(wal_path);

    auto pf = PageFile::create(path).value();
    auto wal = WriteAheadLog::open_or_create(wal_path).value();
    BufferPoolManager bpm(*pf, 64);
    TransactionManager tm(bpm, *wal);

    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();

    SqlExecutor executor(bpm, rm, *tc, *ic);
    executor.set_transaction_manager(&tm);

    EXPECT_TRUE(executor.execute("CREATE TABLE accounts (id INTEGER PRIMARY KEY, balance INTEGER);").is_ok());

    // Insert initial accounts
    EXPECT_TRUE(executor.execute("INSERT INTO accounts VALUES (1, 100);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO accounts VALUES (2, 200);").is_ok());

    // Begin transaction, insert new account, roll back
    EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO accounts VALUES (3, 300);").is_ok());

    // Check plan inside transaction
    {
        auto expl_res = executor.execute("EXPLAIN SELECT * FROM accounts WHERE id = 3;");
        EXPECT_TRUE(expl_res.is_ok());
        std::string plan_text = expl_res.value().rows()[0].get(0).as_text();
        EXPECT_TRUE(plan_text.find("Index Seek") != std::string::npos);
    }

    // Query inside transaction should see id = 3
    {
        auto sel_res = executor.execute("SELECT * FROM accounts WHERE id = 3;");
        EXPECT_TRUE(sel_res.is_ok());
        EXPECT_EQ(sel_res.value().row_count(), 1ULL);
    }

    // Rollback transaction
    EXPECT_TRUE(executor.execute("ROLLBACK;").is_ok());

    // Query outside transaction after rollback: id = 3 must not exist
    {
        auto sel_res = executor.execute("SELECT * FROM accounts WHERE id = 3;");
        EXPECT_TRUE(sel_res.is_ok());
        EXPECT_EQ(sel_res.value().row_count(), 0ULL);
    }

    // Index range scan reflects rolled back state correctly
    {
        auto sel_res = executor.execute("SELECT id FROM accounts WHERE id >= 1;");
        EXPECT_TRUE(sel_res.is_ok());
        EXPECT_EQ(sel_res.value().row_count(), 2ULL);
    }
}

int main() {
    std::cout << "Running Query Optimization Tests...\n";
    run_test("Unselective Cost Comparison", test_unselective_cost_comparison);
    run_test("Optimization with Transactions", test_optimization_with_transactions);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
