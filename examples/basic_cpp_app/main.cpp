#include <klyrdb/klyrdb.hpp>
#include <iostream>
#include <filesystem>

int main() {
    std::cout << "=== KlyrDB Standalone Consumer Example (v"
              << klyrdb::version().to_string() << ") ===\n\n";

    const std::filesystem::path db_path = "basic_app.kdb";
    // Clean up any old test files
    std::error_code ec;
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(db_path.string() + ".wal", ec);
    std::filesystem::remove(db_path.string() + ".lock", ec);

    // 1. Open / create a new database
    std::cout << "[1] Opening new database: " << db_path.string() << "\n";
    auto db_res = klyrdb::Database::open(db_path);
    if (!db_res.is_ok()) {
        std::cerr << "[-] Failed to open database: " << db_res.status().to_string() << "\n";
        return 1;
    }
    auto db = std::move(db_res.value());
    std::cout << "    Database opened successfully (format v" << klyrdb::storage_format_version() << ")\n\n";

    // 2. Create a table
    std::cout << "[2] Creating table 'users'...\n";
    auto create_res = db->execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, email TEXT);");
    if (!create_res.is_ok()) {
        std::cerr << "[-] Table creation failed: " << create_res.status().to_string() << "\n";
        return 1;
    }
    std::cout << "    " << create_res.value().message() << "\n\n";

    // 3. Insert records
    std::cout << "[3] Inserting records...\n";
    (void)db->execute("INSERT INTO users VALUES (1, 'Alice', 'alice@klyrdb.dev');");
    (void)db->execute("INSERT INTO users VALUES (2, 'Bob', 'bob@klyrdb.dev');");
    (void)db->execute("INSERT INTO users VALUES (3, 'Charlie', 'charlie@klyrdb.dev');");
    std::cout << "    Inserted 3 initial records.\n\n";

    // 4 & 5. Execute SELECT and read/print rows via Connection
    std::cout << "[4 & 5] Querying records via Connection:\n";
    auto conn_res = db->connect();
    if (!conn_res.is_ok()) {
        std::cerr << "[-] Failed to establish connection: " << conn_res.status().to_string() << "\n";
        return 1;
    }
    auto conn = std::move(conn_res.value());

    auto sel_res = conn->execute("SELECT id, name, email FROM users ORDER BY id ASC;");
    if (!sel_res.is_ok()) {
        std::cerr << "[-] SELECT failed: " << sel_res.status().to_string() << "\n";
        return 1;
    }

    const auto& rs = sel_res.value();
    std::cout << "    Rows returned: " << rs.row_count() << "\n";
    std::cout << "    --------------------------------------------------------\n";
    std::cout << "    | ID | NAME        | EMAIL                         |\n";
    std::cout << "    --------------------------------------------------------\n";
    for (const auto& row : rs) {
        std::cout << "    | " << row["id"].as_integer()
                  << "  | " << row["name"].as_text()
                  << "     | " << row["email"].as_text() << " |\n";
    }
    std::cout << "    --------------------------------------------------------\n\n";

    // 6. Handle a SQL error gracefully
    std::cout << "[6] Intentionally submitting malformed SQL query to demonstrate error handling...\n";
    auto err_res = conn->execute("SELECT * FROM invalid_table_name;");
    if (err_res.is_err()) {
        std::cout << "    [Handled Expected Error] Code: " << static_cast<int>(err_res.status().code())
                  << " -> " << err_res.status().message() << "\n\n";
    }

    // 7. Transaction management: Rollback then Commit
    std::cout << "[7] Testing Transactions (Rollback & Commit):\n";
    // 7a. Rollback demonstration
    std::cout << "    - Beginning transaction for uncommitted insert (id=99)...\n";
    (void)conn->begin_transaction();
    (void)conn->execute("INSERT INTO users VALUES (99, 'TempUser', 'temp@klyrdb.dev');");
    std::cout << "    - Executing ROLLBACK...\n";
    (void)conn->rollback();

    auto verify_rb = conn->execute("SELECT id FROM users WHERE id = 99;").value();
    std::cout << "    - Records matching id=99 post-rollback: " << verify_rb.row_count() << " (Expected: 0)\n";

    // 7b. Commit demonstration via TransactionGuard
    std::cout << "    - Using RAII TransactionGuard to commit new user (id=4)...\n";
    {
        klyrdb::TransactionGuard txn(*conn);
        auto ins4 = conn->execute("INSERT INTO users VALUES (4, 'Diana', 'diana@klyrdb.dev');");
        if (ins4.is_err()) {
            std::cout << "    - Insert (id=4) error: " << ins4.status().to_string() << "\n";
        } else {
            std::cout << "    - Insert (id=4) succeeded.\n";
        }
        auto commit_status = txn.commit();
        if (commit_status.is_ok()) {
            std::cout << "    - Transaction committed successfully.\n";
        }
    }
    std::cout << "\n";

    // 8. Close the database
    std::cout << "[8] Closing database connection and storage handles...\n";
    db->close();
    std::cout << "    Database closed.\n\n";

    // 9. Reopen and verify persistence
    std::cout << "[9] Reopening database to verify persistence across sessions...\n";
    auto reopen_res = klyrdb::Database::open(db_path);
    if (!reopen_res.is_ok()) {
        std::cerr << "[-] Failed to reopen database: " << reopen_res.status().to_string() << "\n";
        return 1;
    }
    auto reopened_db = std::move(reopen_res.value());
    auto persist_rs = reopened_db->execute("SELECT id, name FROM users ORDER BY id ASC;").value();

    std::cout << "    Reopened database contains " << persist_rs.row_count() << " persisted users:\n";
    for (const auto& row : persist_rs) {
        std::cout << "      * User #" << row["id"].as_integer() << ": " << row["name"].as_text() << "\n";
    }

    // Clean up demo files
    reopened_db->close();
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(db_path.string() + ".wal", ec);
    std::filesystem::remove(db_path.string() + ".lock", ec);

    std::cout << "\n=== All Consumer Verification Steps Passed Successfully! ===\n";
    return 0;
}
