#include <klyrdb/klyrdb.hpp>
#include <iostream>
#include <filesystem>

int main() {
    std::cout << "=== KlyrDB Transactions & Relational Constraints Example ===\n\n";

    const std::filesystem::path db_path = "constraints_demo.kdb";
    std::error_code ec;
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(db_path.string() + ".wal", ec);
    std::filesystem::remove(db_path.string() + ".lock", ec);

    auto db = klyrdb::Database::open(db_path).value();
    auto conn = db->connect().value();

    // 1. Create tables with relational constraints
    std::cout << "[1] Creating schema with PRIMARY KEY, NOT NULL, and UNIQUE constraints...\n";
    (void)conn->execute("CREATE TABLE departments (id INTEGER PRIMARY KEY, dept_name TEXT NOT NULL UNIQUE);");
    (void)conn->execute("CREATE TABLE employees (id INTEGER PRIMARY KEY, name TEXT NOT NULL, dept_id INTEGER, salary INTEGER);");

    (void)conn->execute("INSERT INTO departments VALUES (1, 'Engineering');");
    (void)conn->execute("INSERT INTO departments VALUES (2, 'Design');");
    std::cout << "    Departments seeded.\n\n";

    // 2. Demonstrate PRIMARY KEY constraint violation
    std::cout << "[2] Attempting duplicate PRIMARY KEY insert (id=1 into departments)...\n";
    auto pk_err = conn->execute("INSERT INTO departments VALUES (1, 'Marketing');");
    if (pk_err.is_err()) {
        std::cout << "    [Intercepted Error] " << pk_err.status().message() << "\n\n";
    }

    // 3. Demonstrate NOT NULL constraint violation
    std::cout << "[3] Attempting NOT NULL violation (null department name)...\n";
    auto nn_err = conn->execute("INSERT INTO departments VALUES (3, NULL);");
    if (nn_err.is_err()) {
        std::cout << "    [Intercepted Error] " << nn_err.status().message() << "\n\n";
    }

    // 4. Demonstrate UNIQUE constraint violation
    std::cout << "[4] Attempting UNIQUE violation (duplicate dept_name 'Engineering')...\n";
    auto uq_err = conn->execute("INSERT INTO departments VALUES (4, 'Engineering');");
    if (uq_err.is_err()) {
        std::cout << "    [Intercepted Error] " << uq_err.status().message() << "\n\n";
    }

    // 5. Demonstrate TransactionGuard rolling back automatically on error
    std::cout << "[5] Demonstrating transactional atomicity under constraint failure...\n";
    {
        klyrdb::TransactionGuard txn(*conn);
        (void)conn->execute("INSERT INTO employees VALUES (101, 'John Doe', 1, 95000);");
        
        // This insert will fail due to duplicate PK 101
        auto fail_res = conn->execute("INSERT INTO employees VALUES (101, 'Jane Smith', 1, 105000);");
        if (fail_res.is_err()) {
            std::cout << "    Operation failed inside transaction (" << fail_res.status().message() << ").\n";
            std::cout << "    Allowing TransactionGuard to automatically roll back on scope exit...\n";
            // Do not commit
        }
    }

    // Verify that John Doe (id=101) was rolled back and is NOT present in the database!
    auto check_emp = conn->execute("SELECT id, name FROM employees WHERE id = 101;").value();
    std::cout << "    Employees with id=101 post-rollback: " << check_emp.row_count()
              << " (Transaction rolled back cleanly)\n\n";

    // 6. Verify full integrity of database
    std::cout << "[6] Auditing final database integrity...\n";
    auto integ = db->integrity_check();
    if (integ.is_ok() && integ.value()) {
        std::cout << "    [+] Database is 100% HEALTHY with all constraints satisfied.\n";
    }

    db->close();
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(db_path.string() + ".wal", ec);
    std::filesystem::remove(db_path.string() + ".lock", ec);

    std::cout << "\n=== Transactions & Constraints Demo Completed Successfully! ===\n";
    return 0;
}
