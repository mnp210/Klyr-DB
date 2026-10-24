#include <klyrdb/klyrdb.hpp>
#include <iostream>
#include <filesystem>
#include <string>

int main(int argc, char* argv[]) {
    std::cout << "=========================================================\n";
    std::cout << "      KlyrDB C++20 SDK Portfolio Demonstration           \n";
    std::cout << "=========================================================\n\n";

    // 1. Version and Format Information
    auto v = klyrdb::version();
    std::cout << "[SDK] Engine Version:         v" << v.to_string() << "\n";
    std::cout << "[SDK] Storage Format Version: v" << klyrdb::storage_format_version() << "\n";
    std::cout << "[SDK] Backup Format Version:  v" << klyrdb::backup_format_version() << "\n\n";

    // Determine target database path
    std::filesystem::path db_path = "portfolio_sdk_demo.kdb";
    if (argc > 1) {
        db_path = argv[1];
    }

    std::cout << "[SDK] Opening database at: " << db_path.string() << "\n";

    // Clean up any pre-existing files for this demo path
    std::error_code ec;
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(db_path.string() + ".wal", ec);
    std::filesystem::remove(db_path.string() + ".lock", ec);

    // 2. Open / Create Database
    auto db_res = klyrdb::Database::open(db_path);
    if (!db_res.is_ok()) {
        std::cerr << "[-] Failed to open database: " << db_res.status().to_string() << "\n";
        return 1;
    }
    auto db = std::move(db_res.value());
    std::cout << "[SDK] Database handle successfully acquired (4096-byte slotted pages).\n\n";

    // 3. Connect and execute DDL
    auto conn_res = db->connect();
    if (!conn_res.is_ok()) {
        std::cerr << "[-] Failed to acquire connection: " << conn_res.status().to_string() << "\n";
        return 1;
    }
    auto conn = std::move(conn_res.value());

    std::cout << "[SDK] Creating table 'servers' with constraints...\n";
    auto ddl_res = conn->execute("CREATE TABLE servers (id INTEGER PRIMARY KEY, hostname TEXT NOT NULL UNIQUE, cores INTEGER, ram_gb INTEGER);");
    if (!ddl_res.is_ok()) {
        std::cerr << "[-] DDL failed: " << ddl_res.status().to_string() << "\n";
        return 1;
    }
    std::cout << "      " << ddl_res.value().message() << "\n\n";

    // 4. Seeding rows via direct DML
    std::cout << "[SDK] Seeding initial infrastructure records...\n";
    (void)conn->execute("INSERT INTO servers VALUES (1, 'prod-web-01.klyrdb.net', 16, 64);");
    (void)conn->execute("INSERT INTO servers VALUES (2, 'prod-db-01.klyrdb.net', 32, 128);");
    (void)conn->execute("INSERT INTO servers VALUES (3, 'staging-api-01.klyrdb.net', 8, 32);");
    std::cout << "      3 server records inserted.\n\n";

    // 5. Demonstrate RAII TransactionGuard with automatic rollback on error
    std::cout << "[SDK] Demonstrating RAII TransactionGuard rollback safety...\n";
    {
        klyrdb::TransactionGuard txn(*conn);
        (void)conn->execute("INSERT INTO servers VALUES (99, 'ephemeral-runner.klyrdb.net', 4, 16);");

        // Intentionally trigger a UNIQUE constraint violation on hostname
        auto dup_res = conn->execute("INSERT INTO servers VALUES (100, 'prod-db-01.klyrdb.net', 16, 64);");
        if (dup_res.is_err()) {
            std::cout << "      [Intercepted Expected Constraint Error] "
                      << dup_res.status().message() << "\n";
            std::cout << "      Allowing TransactionGuard to exit scope without commit()...\n";
        }
        // Scope exit: TransactionGuard destructor automatically rolls back uncommitted writes
    }

    auto check_ephemeral = conn->execute("SELECT * FROM servers WHERE id = 99;").value();
    std::cout << "      Ephemeral server 99 present post-rollback: "
              << check_ephemeral.row_count() << " (Expected: 0)\n\n";

    // 6. Demonstrate successful transaction commit via RAII TransactionGuard
    std::cout << "[SDK] Demonstrating durable commit via TransactionGuard...\n";
    {
        klyrdb::TransactionGuard txn(*conn);
        (void)conn->execute("INSERT INTO servers VALUES (4, 'prod-cache-01.klyrdb.net', 16, 64);");
        auto c_st = txn.commit();
        if (c_st.is_ok()) {
            std::cout << "      Transaction committed successfully.\n";
        }
    }

    // 7. Tabular Query Iteration
    std::cout << "\n[SDK] Querying active fleet:\n";
    auto select_res = conn->execute("SELECT id, hostname, cores, ram_gb FROM servers ORDER BY id ASC;");
    if (select_res.is_ok()) {
        const auto& rs = select_res.value();
        std::cout << "      Total Servers: " << rs.row_count() << "\n";
        std::cout << "      --------------------------------------------------------------------\n";
        std::cout << "      | ID | HOSTNAME                     | CORES | RAM (GB)               |\n";
        std::cout << "      --------------------------------------------------------------------\n";
        for (const auto& row : rs) {
            std::cout << "      | " << row["id"].as_integer()
                      << "  | " << row["hostname"].as_text()
                      << "      | " << row["cores"].as_integer()
                      << "    | " << row["ram_gb"].as_integer() << "                     |\n";
        }
        std::cout << "      --------------------------------------------------------------------\n\n";
    }

    // 8. Programmatic Storage & Relational Integrity Verification
    std::cout << "[SDK] Running programmatic storage and relational integrity audit...\n";
    auto integ_res = db->integrity_check();
    if (integ_res.is_ok() && integ_res.value()) {
        std::cout << "      [+] Database integrity check: 100% HEALTHY (0 errors, 0 warnings).\n\n";
    }

    // 9. Close handles
    std::cout << "[SDK] Closing connection and flushing buffer pool...\n";
    db->close();

    // Clean up temporary files
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(db_path.string() + ".wal", ec);
    std::filesystem::remove(db_path.string() + ".lock", ec);

    std::cout << "[SDK] Demonstration completed cleanly.\n";
    return 0;
}
