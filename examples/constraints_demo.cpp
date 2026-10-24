#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/sql/executor.hpp"
#include <iostream>
#include <filesystem>

using namespace klyrdb;

static void print_res(const Result<QueryResult>& res) {
    if (res.is_err()) {
        std::cout << "  [ERROR] " << res.status().message() << "\n";
        return;
    }
    const auto& qr = res.value();
    if (qr.is_message()) {
        std::cout << "  [OK] " << qr.message() << "\n";
    } else if (qr.is_count()) {
        std::cout << "  [OK] " << qr.message() << "\n";
    } else if (qr.is_rows()) {
        std::cout << "  [ROWS] count = " << qr.row_count() << "\n";
        for (const auto& r : qr.rows()) {
            std::cout << "    | ";
            for (size_t i = 0; i < r.column_count(); ++i) {
                std::cout << r.get(i).to_string() << " | ";
            }
            std::cout << "\n";
        }
    }
}

int main() {
    std::cout << "===========================================================\n";
    std::cout << "  KlyrDB Phase 6: Relational Constraints & Integrity Demo  \n";
    std::cout << "===========================================================\n\n";

    std::filesystem::path db_path = "constraints_demo.kdb";
    std::filesystem::path wal_path = "constraints_demo.kdb.wal";

    std::error_code ec;
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(wal_path, ec);

    {
        auto pf = PageFile::create(db_path).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();

        TransactionManager tm(bpm, *wal);
        SqlExecutor executor(bpm, rm, *tc, *ic);
        executor.set_transaction_manager(&tm);

        std::cout << "1. Creating Parent Table 'departments' with PK and UNIQUE constraints:\n";
        print_res(executor.execute("CREATE TABLE departments (id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL);"));

        std::cout << "\n2. Creating Child Table 'employees' with NOT NULL and FOREIGN KEY constraints:\n";
        print_res(executor.execute(
            "CREATE TABLE employees ("
            "  emp_id INTEGER PRIMARY KEY,"
            "  name TEXT NOT NULL,"
            "  email TEXT UNIQUE,"
            "  dept_id INTEGER REFERENCES departments(id)"
            ");"
        ));

        std::cout << "\n3. Populating Departments:\n";
        print_res(executor.execute("INSERT INTO departments VALUES (10, 'Engineering');"));
        print_res(executor.execute("INSERT INTO departments VALUES (20, 'Product');"));

        std::cout << "\n4. Testing PRIMARY KEY duplicate violation on departments:\n";
        print_res(executor.execute("INSERT INTO departments VALUES (10, 'Duplicate Engineering');"));

        std::cout << "\n5. Testing UNIQUE violation on departments name:\n";
        print_res(executor.execute("INSERT INTO departments VALUES (30, 'Engineering');"));

        std::cout << "\n6. Inserting Valid Employees:\n";
        print_res(executor.execute("INSERT INTO employees VALUES (101, 'Alice', 'alice@company.com', 10);"));
        print_res(executor.execute("INSERT INTO employees VALUES (102, 'Bob', 'bob@company.com', 20);"));
        print_res(executor.execute("INSERT INTO employees (emp_id, name) VALUES (103, 'Charlie');"));

        std::cout << "\n7. Testing FOREIGN KEY violation (referencing non-existent dept_id 999):\n";
        print_res(executor.execute("INSERT INTO employees VALUES (104, 'Dave', 'dave@company.com', 999);"));

        std::cout << "\n8. Testing NOT NULL violation (omitting required 'name'):\n";
        print_res(executor.execute("INSERT INTO employees (emp_id, email) VALUES (105, 'noname@company.com');"));

        std::cout << "\n9. Testing RESTRICT on parent DELETE (attempting to delete department 10):\n";
        print_res(executor.execute("DELETE FROM departments WHERE id = 10;"));

        std::cout << "\n10. Testing Index Protection (attempting to DROP index enforcing PRIMARY KEY):\n";
        print_res(executor.execute("DROP INDEX pk_departments_id;"));

        std::cout << "\n11. Reassigning employee Alice to dept 20 and then deleting dept 10:\n";
        print_res(executor.execute("UPDATE employees SET dept_id = 20 WHERE emp_id = 101;"));
        print_res(executor.execute("DELETE FROM departments WHERE id = 10;"));

        std::cout << "\n12. Final Active Employees:\n";
        print_res(executor.execute("SELECT emp_id, name, email, dept_id FROM employees;"));

        std::cout << "\n13. Final Active Departments:\n";
        print_res(executor.execute("SELECT id, name FROM departments;"));
    }

    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(wal_path, ec);

    std::cout << "\nDemo completed successfully.\n";
    return 0;
}
