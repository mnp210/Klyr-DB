#include "test_framework.hpp"
#include "klyrdb/catalog/table_statistics.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/sql/executor.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_explain_analyze_execution() {
    auto path = temp_test_file("test_explain_analyze");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);

    auto pf = PageFile::create(path, false).value();
    auto wal = WriteAheadLog::open_or_create(wal_path).value();
    BufferPoolManager bpm(*pf, 32);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();
    TransactionManager tm(bpm, *wal);

    SqlExecutor executor(bpm, rm, *tc, *ic);
    executor.set_transaction_manager(&tm);

    EXPECT_TRUE(executor.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, age INTEGER, name TEXT);").is_ok());
    EXPECT_TRUE(executor.execute("CREATE INDEX idx_user_age ON users(age);").is_ok());

    EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (1, 25, 'Alice');").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (2, 30, 'Bob');").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (3, 35, 'Charlie');").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (4, 40, 'David');").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (5, 45, 'Eve');").is_ok());

    // 1. EXPLAIN ANALYZE TableScan
    auto res_scan = executor.execute("EXPLAIN ANALYZE SELECT * FROM users;");
    EXPECT_TRUE(res_scan.is_ok());
    auto rows_scan = res_scan.value().rows();
    EXPECT_EQ(rows_scan.size(), 1ULL);
    std::string text_scan = rows_scan[0].get(0).as_text();
    EXPECT_TRUE(text_scan.find("Table Scan") != std::string::npos);
    EXPECT_TRUE(text_scan.find("Execution Summary") != std::string::npos);
    EXPECT_TRUE(text_scan.find("Actual Rows:   5") != std::string::npos);
    EXPECT_TRUE(text_scan.find("Planning Time:") != std::string::npos);
    EXPECT_TRUE(text_scan.find("Exec Time:") != std::string::npos);

    // 2. EXPLAIN ANALYZE IndexSeek
    auto res_seek = executor.execute("EXPLAIN ANALYZE SELECT * FROM users WHERE id = 3;");
    EXPECT_TRUE(res_seek.is_ok());
    auto rows_seek = res_seek.value().rows();
    EXPECT_EQ(rows_seek.size(), 1ULL);
    std::string text_seek = rows_seek[0].get(0).as_text();
    EXPECT_TRUE(text_seek.find("Index Seek") != std::string::npos);
    EXPECT_TRUE(text_seek.find("Execution Summary") != std::string::npos);
    EXPECT_TRUE(text_seek.find("Actual Rows:   1") != std::string::npos);
    EXPECT_TRUE(text_seek.find("Rows Examined: 1") != std::string::npos);

    // 3. EXPLAIN ANALYZE IndexRangeScan
    auto res_range = executor.execute("EXPLAIN ANALYZE SELECT * FROM users WHERE age >= 30 AND age <= 40;");
    EXPECT_TRUE(res_range.is_ok());
    auto rows_range = res_range.value().rows();
    EXPECT_EQ(rows_range.size(), 1ULL);
    std::string text_range = rows_range[0].get(0).as_text();
    EXPECT_TRUE(text_range.find("Index Range Scan") != std::string::npos);
    EXPECT_TRUE(text_range.find("Execution Summary") != std::string::npos);
    EXPECT_TRUE(text_range.find("Actual Rows:   3") != std::string::npos);
}

static void test_table_statistics_collection() {
    auto path = temp_test_file("test_table_stats");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);

    auto pf = PageFile::create(path, false).value();
    auto wal = WriteAheadLog::open_or_create(wal_path).value();
    BufferPoolManager bpm(*pf, 32);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();
    TransactionManager tm(bpm, *wal);

    SqlExecutor executor(bpm, rm, *tc, *ic);
    executor.set_transaction_manager(&tm);

    EXPECT_TRUE(executor.execute("CREATE TABLE orders (order_id INTEGER PRIMARY KEY, customer TEXT, amount INTEGER);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO orders VALUES (101, 'Alice', 50);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO orders VALUES (102, 'Bob', 150);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO orders VALUES (103, 'Alice', 200);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO orders VALUES (104, 'Charlie', 50);").is_ok());

    // Execute SQL ANALYZE
    auto analyze_res = executor.execute("ANALYZE orders;");
    EXPECT_TRUE(analyze_res.is_ok());

    auto stats_opt = TableStatisticsRegistry::instance().get_statistics(path.string(), "orders");
    EXPECT_TRUE(stats_opt.has_value());
    const auto& stats = stats_opt.value();

    EXPECT_EQ(stats.table_name, "orders");
    EXPECT_EQ(stats.total_rows, 4ULL);
    EXPECT_TRUE(stats.total_pages >= 1ULL);

    auto order_id_stats = stats.find_column_stats("order_id");
    EXPECT_TRUE(order_id_stats != nullptr);
    EXPECT_EQ(order_id_stats->distinct_values, 4ULL);
    EXPECT_EQ(order_id_stats->null_count, 0ULL);
    EXPECT_TRUE(order_id_stats->min_value.has_value());
    EXPECT_EQ(order_id_stats->min_value->as_integer(), 101);
    EXPECT_TRUE(order_id_stats->max_value.has_value());
    EXPECT_EQ(order_id_stats->max_value->as_integer(), 104);

    auto cust_stats = stats.find_column_stats("customer");
    EXPECT_TRUE(cust_stats != nullptr);
    EXPECT_EQ(cust_stats->distinct_values, 3ULL); // Alice, Bob, Charlie

    auto amt_stats = stats.find_column_stats("amount");
    EXPECT_TRUE(amt_stats != nullptr);
    EXPECT_EQ(amt_stats->distinct_values, 3ULL); // 50, 150, 200

    // Test JSON and string formatting
    std::string json = stats.to_json();
    EXPECT_TRUE(json.find("\"table_name\":\"orders\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"total_rows\":4") != std::string::npos);

    // Verify QueryPlanner fast row estimation uses cached statistics
    QueryPlanner planner(bpm, *tc, *ic);
    auto tbl = tc->get_table("orders").value();
    EXPECT_EQ(planner.estimate_table_rows(tbl), 4ULL);
}

int main() {
    std::cout << "=== Running Query Profiling & Table Statistics Tests ===\n";

    run_test("test_explain_analyze_execution", test_explain_analyze_execution);
    run_test("test_table_statistics_collection", test_table_statistics_collection);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
