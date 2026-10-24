#include "test_framework.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/catalog/table_catalog.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_table_catalog_create_and_lookup() {
    auto path = temp_test_file("test_tc_create");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 16);

    auto tc = TableCatalog::open_or_create(bpm).value();
    EXPECT_TRUE(tc->catalog_page_id() != INVALID_PAGE_ID);

    // Create table users
    std::vector<ColumnDef> cols = {
        {"id", SqlType::Integer},
        {"name", SqlType::Text},
        {"age", SqlType::Integer}
    };

    auto create_res = tc->create_table("users", cols);
    EXPECT_TRUE(create_res.is_ok());
    auto schema = create_res.value();
    EXPECT_EQ(schema.name, "users");
    EXPECT_EQ(schema.table_id, 1U);
    EXPECT_EQ(schema.columns.size(), 3ULL);

    // Duplicate table name rejection
    auto dup_tbl = tc->create_table("Users", cols); // case-insensitive check
    EXPECT_TRUE(dup_tbl.is_err());
    EXPECT_EQ(dup_tbl.status().code(), StatusCode::TableAlreadyExists);

    // Duplicate column name within table rejection
    std::vector<ColumnDef> bad_cols = {
        {"id", SqlType::Integer},
        {"ID", SqlType::Text}
    };
    auto dup_col = tc->create_table("items", bad_cols);
    EXPECT_TRUE(dup_col.is_err());
    EXPECT_EQ(dup_col.status().code(), StatusCode::DuplicateColumn);

    // Lookup table
    auto lookup_res = tc->get_table("USERS");
    EXPECT_TRUE(lookup_res.is_ok());
    EXPECT_EQ(lookup_res.value().table_id, 1U);
    EXPECT_EQ(lookup_res.value().columns[1].name, "name");
    EXPECT_EQ(lookup_res.value().columns[1].type, SqlType::Text);

    // Attach index
    TableIndexInfo idx_info{"idx_user_age", "age", 2, 42, false};
    EXPECT_TRUE(tc->add_index_to_table("users", idx_info).is_ok());

    auto updated_tbl = tc->get_table("users").value();
    EXPECT_EQ(updated_tbl.indexes.size(), 1ULL);
    EXPECT_EQ(updated_tbl.indexes[0].index_name, "idx_user_age");
    EXPECT_EQ(updated_tbl.indexes[0].root_page_id, 42U);

    // Update index root
    EXPECT_TRUE(tc->update_index_root("users", "idx_user_age", 99).is_ok());
    auto root_updated = tc->get_table("users").value();
    EXPECT_EQ(root_updated.indexes[0].root_page_id, 99U);
}

static void test_table_catalog_persistence() {
    auto path = temp_test_file("test_tc_persist");
    ScopedTempFile cleanup(path);

    // Session 1: Create multiple tables and indexes
    {
        auto pf = PageFile::create(path).value();
        BufferPoolManager bpm(*pf, 16);
        auto tc = TableCatalog::open_or_create(bpm).value();

        auto t1 = tc->create_table("employees", {
            {"emp_id", SqlType::Integer},
            {"dept", SqlType::Text}
        }).value();

        auto t2 = tc->create_table("departments", {
            {"dept_id", SqlType::Integer},
            {"dept_name", SqlType::Text}
        }).value();

        EXPECT_EQ(t1.table_id, 1U);
        EXPECT_EQ(t2.table_id, 2U);

        TableIndexInfo idx{"idx_emp_id", "emp_id", 0, 77, true};
        EXPECT_TRUE(tc->add_index_to_table("employees", idx).is_ok());

        EXPECT_TRUE(bpm.flush_all_pages().is_ok());
    }

    // Session 2: Reopen database and verify all catalog data recovered
    {
        auto pf = PageFile::open(path).value();
        BufferPoolManager bpm(*pf, 16);
        auto tc = TableCatalog::open_or_create(bpm).value();

        auto tables = tc->list_tables();
        EXPECT_EQ(tables.size(), 2ULL);

        auto emp_res = tc->get_table("EMPLOYEES");
        EXPECT_TRUE(emp_res.is_ok());
        const auto& emp = emp_res.value();
        EXPECT_EQ(emp.table_id, 1U);
        EXPECT_EQ(emp.columns.size(), 2ULL);
        EXPECT_EQ(emp.columns[0].name, "emp_id");
        EXPECT_EQ(emp.columns[0].type, SqlType::Integer);
        EXPECT_EQ(emp.indexes.size(), 1ULL);
        EXPECT_EQ(emp.indexes[0].index_name, "idx_emp_id");
        EXPECT_EQ(emp.indexes[0].root_page_id, 77U);
        EXPECT_TRUE(emp.indexes[0].is_unique);

        auto dept_res = tc->get_table("departments");
        EXPECT_TRUE(dept_res.is_ok());
        EXPECT_EQ(dept_res.value().table_id, 2U);
        EXPECT_EQ(dept_res.value().columns[1].name, "dept_name");
        EXPECT_EQ(dept_res.value().columns[1].type, SqlType::Text);
    }
}

int main() {
    std::cout << "Running Table Catalog Tests...\n";
    run_test("Table Catalog Create and Lookup", test_table_catalog_create_and_lookup);
    run_test("Table Catalog Persistence", test_table_catalog_persistence);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
