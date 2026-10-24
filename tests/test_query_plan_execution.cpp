#include "test_framework.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/sql/executor.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_index_range_scan_execution() {
    auto path = temp_test_file("test_range_exec");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 64);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();
    SqlExecutor executor(bpm, rm, *tc, *ic);

    EXPECT_TRUE(executor.execute("CREATE TABLE scores (id INTEGER PRIMARY KEY, player TEXT, points INTEGER);").is_ok());
    EXPECT_TRUE(executor.execute("CREATE INDEX idx_scores_points ON scores(points);").is_ok());

    // Insert 10 rows: points 10, 20, 30, ..., 100
    for (int i = 1; i <= 10; ++i) {
        std::string ins = "INSERT INTO scores VALUES (" + std::to_string(i) + ", 'Player" + std::to_string(i) + "', " + std::to_string(i * 10) + ");";
        EXPECT_TRUE(executor.execute(ins).is_ok());
    }

    // 1. Lower bound range: points >= 70 (points 70, 80, 90, 100 -> 4 rows)
    {
        auto res = executor.execute("SELECT points FROM scores WHERE points >= 70;");
        EXPECT_TRUE(res.is_ok());
        const auto& qr = res.value();
        EXPECT_EQ(qr.row_count(), 4ULL);
    }

    // 2. Strict lower bound: points > 70 (points 80, 90, 100 -> 3 rows)
    {
        auto res = executor.execute("SELECT points FROM scores WHERE points > 70;");
        EXPECT_TRUE(res.is_ok());
        const auto& qr = res.value();
        EXPECT_EQ(qr.row_count(), 3ULL);
    }

    // 3. Upper bound: points <= 40 (points 10, 20, 30, 40 -> 4 rows)
    {
        auto res = executor.execute("SELECT points FROM scores WHERE points <= 40;");
        EXPECT_TRUE(res.is_ok());
        const auto& qr = res.value();
        EXPECT_EQ(qr.row_count(), 4ULL);
    }

    // 4. Strict upper bound: points < 40 (points 10, 20, 30 -> 3 rows)
    {
        auto res = executor.execute("SELECT points FROM scores WHERE points < 40;");
        EXPECT_TRUE(res.is_ok());
        const auto& qr = res.value();
        EXPECT_EQ(qr.row_count(), 3ULL);
    }

    // 5. Two-sided range: points > 30 AND points <= 70 (points 40, 50, 60, 70 -> 4 rows)
    {
        auto res = executor.execute("SELECT points FROM scores WHERE points > 30 AND points <= 70;");
        EXPECT_TRUE(res.is_ok());
        const auto& qr = res.value();
        EXPECT_EQ(qr.row_count(), 4ULL);
        EXPECT_EQ(qr.rows()[0].get(0).as_integer(), 40);
        EXPECT_EQ(qr.rows()[1].get(0).as_integer(), 50);
        EXPECT_EQ(qr.rows()[2].get(0).as_integer(), 60);
        EXPECT_EQ(qr.rows()[3].get(0).as_integer(), 70);
    }
}

static void test_residual_filters_and_duplicate_keys() {
    auto path = temp_test_file("test_residual_exec");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 64);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();
    SqlExecutor executor(bpm, rm, *tc, *ic);

    EXPECT_TRUE(executor.execute("CREATE TABLE products (id INTEGER, category TEXT, price INTEGER);").is_ok());
    EXPECT_TRUE(executor.execute("CREATE INDEX idx_products_price ON products(price);").is_ok());

    // Insert records with duplicate prices:
    // price 100: Books, Electronics, Clothing
    // price 200: Books, Electronics
    EXPECT_TRUE(executor.execute("INSERT INTO products VALUES (1, 'Books', 100);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO products VALUES (2, 'Electronics', 100);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO products VALUES (3, 'Clothing', 100);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO products VALUES (4, 'Books', 200);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO products VALUES (5, 'Electronics', 200);").is_ok());

    // 1. Index Seek on non-unique price = 100 retrieves all 3 duplicates
    {
        auto res = executor.execute("SELECT id, category FROM products WHERE price = 100;");
        EXPECT_TRUE(res.is_ok());
        EXPECT_EQ(res.value().row_count(), 3ULL);
    }

    // 2. Index Seek on price = 100 with residual filter category = 'Electronics'
    {
        auto res = executor.execute("SELECT id FROM products WHERE price = 100 AND category = 'Electronics';");
        EXPECT_TRUE(res.is_ok());
        const auto& qr = res.value();
        EXPECT_EQ(qr.row_count(), 1ULL);
        EXPECT_EQ(qr.rows()[0].get(0).as_integer(), 2);
    }

    // 3. Range scan with residual filter: price >= 100 AND category = 'Books'
    {
        auto res = executor.execute("SELECT id, price FROM products WHERE price >= 100 AND category = 'Books';");
        EXPECT_TRUE(res.is_ok());
        const auto& qr = res.value();
        EXPECT_EQ(qr.row_count(), 2ULL);
        EXPECT_EQ(qr.rows()[0].get(0).as_integer(), 1);
        EXPECT_EQ(qr.rows()[1].get(0).as_integer(), 4);
    }
}

static void test_index_ordering_and_limit() {
    auto path = temp_test_file("test_index_ordering");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 64);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();
    SqlExecutor executor(bpm, rm, *tc, *ic);

    EXPECT_TRUE(executor.execute("CREATE TABLE numbers (val INTEGER PRIMARY KEY);").is_ok());

    // Insert out of order: 50, 10, 40, 20, 30
    EXPECT_TRUE(executor.execute("INSERT INTO numbers VALUES (50);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO numbers VALUES (10);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO numbers VALUES (40);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO numbers VALUES (20);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO numbers VALUES (30);").is_ok());

    // 1. ORDER BY ASC
    {
        auto res = executor.execute("SELECT val FROM numbers ORDER BY val ASC;");
        EXPECT_TRUE(res.is_ok());
        const auto& qr = res.value();
        EXPECT_EQ(qr.row_count(), 5ULL);
        EXPECT_EQ(qr.rows()[0].get(0).as_integer(), 10);
        EXPECT_EQ(qr.rows()[1].get(0).as_integer(), 20);
        EXPECT_EQ(qr.rows()[2].get(0).as_integer(), 30);
        EXPECT_EQ(qr.rows()[3].get(0).as_integer(), 40);
        EXPECT_EQ(qr.rows()[4].get(0).as_integer(), 50);
    }

    // 2. ORDER BY DESC
    {
        auto res = executor.execute("SELECT val FROM numbers ORDER BY val DESC;");
        EXPECT_TRUE(res.is_ok());
        const auto& qr = res.value();
        EXPECT_EQ(qr.row_count(), 5ULL);
        EXPECT_EQ(qr.rows()[0].get(0).as_integer(), 50);
        EXPECT_EQ(qr.rows()[1].get(0).as_integer(), 40);
        EXPECT_EQ(qr.rows()[2].get(0).as_integer(), 30);
        EXPECT_EQ(qr.rows()[3].get(0).as_integer(), 20);
        EXPECT_EQ(qr.rows()[4].get(0).as_integer(), 10);
    }

    // 3. ORDER BY ASC LIMIT 2
    {
        auto res = executor.execute("SELECT val FROM numbers ORDER BY val ASC LIMIT 2;");
        EXPECT_TRUE(res.is_ok());
        const auto& qr = res.value();
        EXPECT_EQ(qr.row_count(), 2ULL);
        EXPECT_EQ(qr.rows()[0].get(0).as_integer(), 10);
        EXPECT_EQ(qr.rows()[1].get(0).as_integer(), 20);
    }

    // 4. ORDER BY DESC LIMIT 2
    {
        auto res = executor.execute("SELECT val FROM numbers ORDER BY val DESC LIMIT 2;");
        EXPECT_TRUE(res.is_ok());
        const auto& qr = res.value();
        EXPECT_EQ(qr.row_count(), 2ULL);
        EXPECT_EQ(qr.rows()[0].get(0).as_integer(), 50);
        EXPECT_EQ(qr.rows()[1].get(0).as_integer(), 40);
    }
}

int main() {
    std::cout << "Running Query Plan Execution Tests...\n";
    run_test("Index Range Scan Execution", test_index_range_scan_execution);
    run_test("Residual Filters and Duplicate Keys", test_residual_filters_and_duplicate_keys);
    run_test("Index Ordering and Limit Pushdown", test_index_ordering_and_limit);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
