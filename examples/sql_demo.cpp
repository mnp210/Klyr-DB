#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/sql/executor.hpp"
#include "klyrdb/sql/sql_shell.hpp"
#include <iostream>
#include <filesystem>

using namespace klyrdb;

void run_sql(SqlExecutor& executor, std::string_view sql) {
    std::cout << "SQL> " << sql << "\n";
    auto res = executor.execute(sql);
    if (res.is_err()) {
        std::cout << "Error: " << res.status().message() << "\n\n";
    } else {
        SqlShell::print_result(res.value(), std::cout);
        std::cout << "\n";
    }
}

int main() {
    std::string db_path = "phase4_demo.kdb";
    std::error_code ec;
    std::filesystem::remove(db_path, ec);

    std::cout << "=========================================================\n";
    std::cout << " KlyrDB Phase 4: SQL Engine & Table Catalog Demonstration\n";
    std::cout << "=========================================================\n\n";

    // Session 1: Schema definition, insertions, indexing, updates, deletions
    {
        std::cout << "--- Session 1: Creating database and executing SQL statements ---\n\n";
        auto pf = PageFile::create(db_path).value();
        BufferPoolManager bpm(*pf, 64);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        // 1. Create Tables
        run_sql(executor, "CREATE TABLE users (id INTEGER, name TEXT, age INTEGER);");
        run_sql(executor, "CREATE TABLE products (sku INTEGER, title TEXT, price INTEGER);");

        // 2. Data Insertion
        run_sql(executor, "INSERT INTO users VALUES (1, 'Alice', 24);");
        run_sql(executor, "INSERT INTO users (id, name, age) VALUES (2, 'Bob', 29);");
        run_sql(executor, "INSERT INTO users VALUES (3, 'Charlie', 35);");
        run_sql(executor, "INSERT INTO users (age, name, id) VALUES (19, 'Diana', 4);");
        run_sql(executor, "INSERT INTO users VALUES (5, 'Evan', 31);");

        run_sql(executor, "INSERT INTO products VALUES (101, 'Mechanical Keyboard', 120);");
        run_sql(executor, "INSERT INTO products VALUES (102, '4K Monitor', 450);");
        run_sql(executor, "INSERT INTO products VALUES (103, 'Wireless Mouse', 60);");
        run_sql(executor, "INSERT INTO products VALUES (104, 'Noise Cancelling Headphones', 280);");

        // 3. Select with Projections and Filters
        run_sql(executor, "SELECT * FROM users;");
        run_sql(executor, "SELECT name, age FROM users WHERE age >= 25 AND age < 35;");
        run_sql(executor, "SELECT name, age FROM users ORDER BY age DESC LIMIT 3;");

        // 4. Create B+ Tree Index on Existing Data
        std::cout << "Creating persistent B+ Tree index on users(id)...\n";
        run_sql(executor, "CREATE INDEX idx_users_id ON users (id);");

        // 5. Indexed Point Lookup
        std::cout << "Executing index-accelerated point query:\n";
        run_sql(executor, "SELECT * FROM users WHERE id = 3;");

        // 6. UPDATE Statement
        std::cout << "Updating user age (with WHERE safety guard):\n";
        run_sql(executor, "UPDATE users SET age = 25 WHERE id = 1;");
        run_sql(executor, "SELECT * FROM users WHERE id = 1;");

        // 7. Safety Rejection Test
        std::cout << "Testing safety rejection of UPDATE without WHERE:\n";
        run_sql(executor, "UPDATE users SET age = 99;");

        // 8. DELETE Statement
        std::cout << "Deleting user (with WHERE safety guard):\n";
        run_sql(executor, "DELETE FROM users WHERE id = 4;");
        run_sql(executor, "SELECT * FROM users;");

        // 9. Multi-Table Query
        run_sql(executor, "SELECT title, price FROM products WHERE price > 100 ORDER BY price ASC;");

        // Flush pages to disk
        (void)bpm.flush_all_pages();
        std::cout << "Flushed all buffer pool pages to disk. Closing database session.\n\n";
    }

    // Session 2: Database Reopen & Recovery
    {
        std::cout << "--- Session 2: Reopening database from disk and verifying persistence ---\n\n";
        auto pf = PageFile::open(db_path).value();
        BufferPoolManager bpm(*pf, 64);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        SqlShell shell(executor);
        std::cout << "Shell inspection of recovered catalogs:\n";
        shell.execute_line(".tables");
        shell.execute_line(".schema");
        shell.execute_line(".indexes");

        std::cout << "Recovered data queries:\n";
        run_sql(executor, "SELECT * FROM users ORDER BY id ASC;");
        run_sql(executor, "SELECT * FROM products ORDER BY price DESC;");
        run_sql(executor, "SELECT * FROM users WHERE id = 2;");
    }

    std::filesystem::remove(db_path, ec);
    std::cout << "=========================================================\n";
    std::cout << " Demonstration finished successfully!\n";
    std::cout << "=========================================================\n";
    return 0;
}
