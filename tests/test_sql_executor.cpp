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

static void test_sql_crud_and_indexing() {
    auto path = temp_test_file("test_sql_crud");
    ScopedTempFile cleanup(path);

    auto pf = PageFile::create(path).value();
    BufferPoolManager bpm(*pf, 32);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();

    SqlExecutor executor(bpm, rm, *tc, *ic);

    // 1. CREATE TABLE
    auto res_ct = executor.execute("CREATE TABLE users (id INTEGER, name TEXT, age INTEGER);");
    EXPECT_TRUE(res_ct.is_ok());
    EXPECT_TRUE(res_ct.value().is_message());

    // 2. INSERT statements
    EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (1, 'Alice', 22);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO users (id, name, age) VALUES (2, 'Bob', 25);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (3, 'Charlie', 30);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO users (age, name, id) VALUES (19, 'Dave', 4);").is_ok());

    // 3. SELECT *
    {
        auto sel_res = executor.execute("SELECT * FROM users;");
        EXPECT_TRUE(sel_res.is_ok());
        const auto& qr = sel_res.value();
        EXPECT_EQ(qr.row_count(), 4ULL);
        EXPECT_EQ(qr.column_names().size(), 3ULL);
    }

    // 4. SELECT with WHERE and projection
    {
        auto sel_res = executor.execute("SELECT name, age FROM users WHERE age >= 25;");
        EXPECT_TRUE(sel_res.is_ok());
        const auto& qr = sel_res.value();
        EXPECT_EQ(qr.row_count(), 2ULL);
        EXPECT_EQ(qr.column_names().size(), 2ULL);
        EXPECT_EQ(qr.column_names()[0], "name");
        EXPECT_EQ(qr.column_names()[1], "age");
    }

    // 5. ORDER BY and LIMIT
    {
        auto sel_res = executor.execute("SELECT name, age FROM users ORDER BY age DESC LIMIT 2;");
        EXPECT_TRUE(sel_res.is_ok());
        const auto& qr = sel_res.value();
        EXPECT_EQ(qr.row_count(), 2ULL);
        EXPECT_EQ(qr.rows()[0].get(0).as_text(), "Charlie");
        EXPECT_EQ(qr.rows()[0].get(1).as_integer(), 30);
        EXPECT_EQ(qr.rows()[1].get(0).as_text(), "Bob");
        EXPECT_EQ(qr.rows()[1].get(1).as_integer(), 25);
    }

    // 6. CREATE INDEX on existing data
    auto idx_res = executor.execute("CREATE INDEX idx_user_id ON users (id);");
    EXPECT_TRUE(idx_res.is_ok());

    // 7. Indexed point lookup: WHERE id = 2
    {
        auto sel_res = executor.execute("SELECT * FROM users WHERE id = 2;");
        EXPECT_TRUE(sel_res.is_ok());
        const auto& qr = sel_res.value();
        EXPECT_EQ(qr.row_count(), 1ULL);
        EXPECT_EQ(qr.rows()[0].get(1).as_text(), "Bob");
        EXPECT_EQ(qr.rows()[0].get(2).as_integer(), 25);
    }

    // 8. UPDATE with WHERE
    {
        auto upd_res = executor.execute("UPDATE users SET age = 26 WHERE id = 2;");
        EXPECT_TRUE(upd_res.is_ok());
        EXPECT_EQ(upd_res.value().rows_affected(), 1ULL);

        auto sel_res = executor.execute("SELECT age FROM users WHERE id = 2;");
        EXPECT_TRUE(sel_res.is_ok());
        EXPECT_EQ(sel_res.value().rows()[0].get(0).as_integer(), 26);
    }

    // 9. DELETE with WHERE
    {
        auto del_res = executor.execute("DELETE FROM users WHERE id = 1;");
        EXPECT_TRUE(del_res.is_ok());
        EXPECT_EQ(del_res.value().rows_affected(), 1ULL);

        auto sel_res = executor.execute("SELECT * FROM users WHERE id = 1;");
        EXPECT_TRUE(sel_res.is_ok());
        EXPECT_EQ(sel_res.value().row_count(), 0ULL);

        auto count_res = executor.execute("SELECT * FROM users;");
        EXPECT_TRUE(count_res.is_ok());
        EXPECT_EQ(count_res.value().row_count(), 3ULL);
    }
}

static void test_multi_table_and_reopen_persistence() {
    auto path = temp_test_file("test_multi_table_persist");
    ScopedTempFile cleanup(path);

    // Session 1: Create multiple tables, populate, create index
    {
        auto pf = PageFile::create(path).value();
        BufferPoolManager bpm(*pf, 32);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        EXPECT_TRUE(executor.execute("CREATE TABLE users (id INTEGER, name TEXT);").is_ok());
        EXPECT_TRUE(executor.execute("CREATE TABLE products (sku INTEGER, title TEXT, price INTEGER);").is_ok());

        EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (10, 'Alice');").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (20, 'Bob');").is_ok());

        EXPECT_TRUE(executor.execute("INSERT INTO products VALUES (101, 'Laptop', 1200);").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO products VALUES (102, 'Phone', 800);").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO products VALUES (103, 'Headphones', 150);").is_ok());

        EXPECT_TRUE(executor.execute("CREATE INDEX idx_prod_sku ON products (sku);").is_ok());

        EXPECT_TRUE(bpm.flush_all_pages().is_ok());
    }

    // Session 2: Reopen database and verify both tables and index are functional
    {
        auto pf = PageFile::open(path).value();
        BufferPoolManager bpm(*pf, 32);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        // Query users
        auto u_res = executor.execute("SELECT * FROM users ORDER BY id ASC;");
        EXPECT_TRUE(u_res.is_ok());
        EXPECT_EQ(u_res.value().row_count(), 2ULL);
        EXPECT_EQ(u_res.value().rows()[0].get(1).as_text(), "Alice");
        EXPECT_EQ(u_res.value().rows()[1].get(1).as_text(), "Bob");

        // Query products via index
        auto p_res = executor.execute("SELECT title, price FROM products WHERE sku = 102;");
        EXPECT_TRUE(p_res.is_ok());
        EXPECT_EQ(p_res.value().row_count(), 1ULL);
        EXPECT_EQ(p_res.value().rows()[0].get(0).as_text(), "Phone");
        EXPECT_EQ(p_res.value().rows()[0].get(1).as_integer(), 800);

        // Filter products with multiple WHERE conditions
        auto cheap_res = executor.execute("SELECT title FROM products WHERE price < 1000 AND price > 200;");
        EXPECT_TRUE(cheap_res.is_ok());
        EXPECT_EQ(cheap_res.value().row_count(), 1ULL);
        EXPECT_EQ(cheap_res.value().rows()[0].get(0).as_text(), "Phone");
    }
}

int main() {
    std::cout << "Running SQL Executor Tests...\n";
    run_test("SQL CRUD and Indexing", test_sql_crud_and_indexing);
    run_test("Multi-Table and Reopen Persistence", test_multi_table_and_reopen_persistence);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
