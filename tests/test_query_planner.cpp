#include "test_framework.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/sql/query_planner.hpp"
#include "klyrdb/sql/parser.hpp"
#include "klyrdb/sql/executor.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_planner_access_paths() {
    auto path = temp_test_file("test_planner_paths");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 32);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();
    SqlExecutor executor(bpm, rm, *tc, *ic);

    EXPECT_TRUE(executor.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, age INTEGER, name TEXT);").is_ok());
    EXPECT_TRUE(executor.execute("CREATE INDEX idx_users_age ON users(age);").is_ok());

    QueryPlanner planner(bpm, *tc, *ic);

    // 1. Table Scan when no predicates
    {
        auto stmt_res = SqlParser::parse_sql("SELECT * FROM users;");
        EXPECT_TRUE(stmt_res.is_ok());
        auto* sel_stmt = static_cast<SelectStatement*>(stmt_res.value().get());
        auto plan_res = planner.plan_select(*sel_stmt);
        EXPECT_TRUE(plan_res.is_ok());
        const auto& plan = plan_res.value();
        EXPECT_EQ(plan.access_path, AccessPathType::TableScan);
        EXPECT_FALSE(plan.index_access.has_value());
    }

    // 2. Table Scan when predicate on non-indexed column
    {
        auto stmt_res = SqlParser::parse_sql("SELECT * FROM users WHERE name = 'Alice';");
        EXPECT_TRUE(stmt_res.is_ok());
        auto* sel_stmt = static_cast<SelectStatement*>(stmt_res.value().get());
        auto plan_res = planner.plan_select(*sel_stmt);
        EXPECT_TRUE(plan_res.is_ok());
        const auto& plan = plan_res.value();
        EXPECT_EQ(plan.access_path, AccessPathType::TableScan);
        EXPECT_EQ(plan.residual_filters.size(), 1ULL);
    }

    // 3. Index Seek on primary key (id)
    {
        auto stmt_res = SqlParser::parse_sql("SELECT * FROM users WHERE id = 42;");
        EXPECT_TRUE(stmt_res.is_ok());
        auto* sel_stmt = static_cast<SelectStatement*>(stmt_res.value().get());
        auto plan_res = planner.plan_select(*sel_stmt);
        EXPECT_TRUE(plan_res.is_ok());
        const auto& plan = plan_res.value();
        EXPECT_EQ(plan.access_path, AccessPathType::IndexSeek);
        EXPECT_TRUE(plan.index_access.has_value());
        EXPECT_EQ(plan.index_access->column_name, "id");
        EXPECT_EQ(plan.index_access->exact_value.as_integer(), 42);
        EXPECT_TRUE(plan.residual_filters.empty());
    }

    // 4. Index Range Scan on secondary index (age)
    {
        auto stmt_res = SqlParser::parse_sql("SELECT * FROM users WHERE age >= 21 AND age < 65;");
        EXPECT_TRUE(stmt_res.is_ok());
        auto* sel_stmt = static_cast<SelectStatement*>(stmt_res.value().get());
        auto plan_res = planner.plan_select(*sel_stmt);
        EXPECT_TRUE(plan_res.is_ok());
        const auto& plan = plan_res.value();
        EXPECT_EQ(plan.access_path, AccessPathType::IndexRangeScan);
        EXPECT_TRUE(plan.index_access.has_value());
        EXPECT_EQ(plan.index_access->column_name, "age");
        EXPECT_TRUE(plan.index_access->is_range);
        EXPECT_TRUE(plan.index_access->low_value.has_value());
        EXPECT_EQ(plan.index_access->low_value->as_integer(), 21);
        EXPECT_TRUE(plan.index_access->inclusive_low);
        EXPECT_TRUE(plan.index_access->high_value.has_value());
        EXPECT_EQ(plan.index_access->high_value->as_integer(), 65);
        EXPECT_FALSE(plan.index_access->inclusive_high);
        EXPECT_TRUE(plan.residual_filters.empty());
    }

    // 5. Conjunction with residual filter
    {
        auto stmt_res = SqlParser::parse_sql("SELECT * FROM users WHERE id = 100 AND name = 'Bob';");
        EXPECT_TRUE(stmt_res.is_ok());
        auto* sel_stmt = static_cast<SelectStatement*>(stmt_res.value().get());
        auto plan_res = planner.plan_select(*sel_stmt);
        EXPECT_TRUE(plan_res.is_ok());
        const auto& plan = plan_res.value();
        EXPECT_EQ(plan.access_path, AccessPathType::IndexSeek);
        EXPECT_TRUE(plan.index_access.has_value());
        EXPECT_EQ(plan.index_access->column_name, "id");
        EXPECT_EQ(plan.residual_filters.size(), 1ULL);
        EXPECT_EQ(plan.residual_filters[0].column_name, "name");
    }
}

static void test_planner_order_by_and_limit() {
    auto path = temp_test_file("test_planner_order");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 32);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();
    SqlExecutor executor(bpm, rm, *tc, *ic);

    EXPECT_TRUE(executor.execute("CREATE TABLE items (id INTEGER PRIMARY KEY, val INTEGER);").is_ok());

    QueryPlanner planner(bpm, *tc, *ic);

    // 1. ORDER BY indexed column ASC without WHERE
    {
        auto stmt_res = SqlParser::parse_sql("SELECT * FROM items ORDER BY id ASC LIMIT 5;");
        EXPECT_TRUE(stmt_res.is_ok());
        auto* sel_stmt = static_cast<SelectStatement*>(stmt_res.value().get());
        auto plan_res = planner.plan_select(*sel_stmt);
        EXPECT_TRUE(plan_res.is_ok());
        const auto& plan = plan_res.value();
        EXPECT_EQ(plan.access_path, AccessPathType::IndexRangeScan);
        EXPECT_TRUE(plan.order_by_satisfied_by_index);
        EXPECT_FALSE(plan.scan_descending);
        EXPECT_TRUE(plan.limit_pushed_down);
    }

    // 2. ORDER BY indexed column DESC without WHERE
    {
        auto stmt_res = SqlParser::parse_sql("SELECT * FROM items ORDER BY id DESC LIMIT 10;");
        EXPECT_TRUE(stmt_res.is_ok());
        auto* sel_stmt = static_cast<SelectStatement*>(stmt_res.value().get());
        auto plan_res = planner.plan_select(*sel_stmt);
        EXPECT_TRUE(plan_res.is_ok());
        const auto& plan = plan_res.value();
        EXPECT_EQ(plan.access_path, AccessPathType::IndexRangeScan);
        EXPECT_TRUE(plan.order_by_satisfied_by_index);
        EXPECT_TRUE(plan.scan_descending);
        EXPECT_TRUE(plan.limit_pushed_down);
    }

    // 3. ORDER BY non-indexed column -> explicit sort, limit NOT pushed down to scan
    {
        auto stmt_res = SqlParser::parse_sql("SELECT * FROM items ORDER BY val ASC LIMIT 5;");
        EXPECT_TRUE(stmt_res.is_ok());
        auto* sel_stmt = static_cast<SelectStatement*>(stmt_res.value().get());
        auto plan_res = planner.plan_select(*sel_stmt);
        EXPECT_TRUE(plan_res.is_ok());
        const auto& plan = plan_res.value();
        EXPECT_EQ(plan.access_path, AccessPathType::TableScan);
        EXPECT_FALSE(plan.order_by_satisfied_by_index);
        EXPECT_FALSE(plan.limit_pushed_down);
    }
}

int main() {
    std::cout << "Running Query Planner Tests...\n";
    run_test("Planner Access Paths", test_planner_access_paths);
    run_test("Planner ORDER BY and LIMIT", test_planner_order_by_and_limit);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
