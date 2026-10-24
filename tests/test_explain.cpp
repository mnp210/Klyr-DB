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

static void test_explain_select_queries() {
    auto path = temp_test_file("test_explain_select");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 32);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();
    SqlExecutor executor(bpm, rm, *tc, *ic);

    EXPECT_TRUE(executor.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER);").is_ok());
    EXPECT_TRUE(executor.execute("CREATE INDEX idx_users_age ON users(age);").is_ok());

    // 1. EXPLAIN SELECT * FROM users
    {
        auto res = executor.execute("EXPLAIN SELECT * FROM users;");
        EXPECT_TRUE(res.is_ok());
        const auto& qr = res.value();
        EXPECT_TRUE(qr.is_rows());
        EXPECT_EQ(qr.column_names().size(), 1ULL);
        EXPECT_EQ(qr.column_names()[0], "QUERY PLAN");
        EXPECT_EQ(qr.row_count(), 1ULL);

        std::string plan_str = qr.rows()[0].get(0).as_text();
        EXPECT_TRUE(plan_str.find("Table Scan") != std::string::npos);
        EXPECT_TRUE(plan_str.find("Table: users") != std::string::npos);
        EXPECT_TRUE(plan_str.find("Estimated Cost:") != std::string::npos);
    }

    // 2. EXPLAIN QUERY PLAN SELECT on indexed PK
    {
        auto res = executor.execute("EXPLAIN QUERY PLAN SELECT name FROM users WHERE id = 42;");
        EXPECT_TRUE(res.is_ok());
        const auto& qr = res.value();
        EXPECT_EQ(qr.row_count(), 1ULL);

        std::string plan_str = qr.rows()[0].get(0).as_text();
        EXPECT_TRUE(plan_str.find("Index Seek") != std::string::npos);
        EXPECT_TRUE(plan_str.find("id = 42") != std::string::npos);
    }

    // 3. EXPLAIN with Range Scan + Residual Filter + Order By + Limit
    {
        auto res = executor.execute("EXPLAIN SELECT * FROM users WHERE age >= 21 AND name = 'Alice' ORDER BY age ASC LIMIT 10;");
        EXPECT_TRUE(res.is_ok());
        const auto& qr = res.value();
        EXPECT_EQ(qr.row_count(), 1ULL);

        std::string plan_str = qr.rows()[0].get(0).as_text();
        EXPECT_TRUE(plan_str.find("Index Range Scan") != std::string::npos);
        EXPECT_TRUE(plan_str.find("idx_users_age") != std::string::npos);
        EXPECT_TRUE(plan_str.find("Residual Filter: name = Alice") != std::string::npos);
        EXPECT_TRUE(plan_str.find("Order: age ASC (Satisfied by index)") != std::string::npos);
        EXPECT_TRUE(plan_str.find("Limit: 10 (Pushed down)") != std::string::npos);
    }
}

static void test_explain_mutation_rejection() {
    auto path = temp_test_file("test_explain_reject");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 32);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();
    SqlExecutor executor(bpm, rm, *tc, *ic);

    EXPECT_TRUE(executor.execute("CREATE TABLE items (id INTEGER, val TEXT);").is_ok());

    // EXPLAIN on INSERT should be rejected
    auto res_ins = executor.execute("EXPLAIN INSERT INTO items VALUES (1, 'test');");
    EXPECT_FALSE(res_ins.is_ok());

    // EXPLAIN on UPDATE should be rejected
    auto res_upd = executor.execute("EXPLAIN UPDATE items SET val = 'updated' WHERE id = 1;");
    EXPECT_FALSE(res_upd.is_ok());

    // EXPLAIN on DELETE should be rejected
    auto res_del = executor.execute("EXPLAIN DELETE FROM items WHERE id = 1;");
    EXPECT_FALSE(res_del.is_ok());

    // EXPLAIN on CREATE TABLE should be rejected
    auto res_crt = executor.execute("EXPLAIN CREATE TABLE foo (id INTEGER);");
    EXPECT_FALSE(res_crt.is_ok());
}

int main() {
    std::cout << "Running EXPLAIN Tests...\n";
    run_test("EXPLAIN SELECT Queries", test_explain_select_queries);
    run_test("EXPLAIN Mutation Rejection", test_explain_mutation_rejection);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
