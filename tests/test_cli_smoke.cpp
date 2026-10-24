#include "test_framework.hpp"
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace klyrdb::testing;

static std::string find_klyrdb_executable() {
    std::vector<std::filesystem::path> candidates = {
        "build/Debug/klyrdb.exe",
        "build/Release/klyrdb.exe",
        "build/klyrdb.exe",
        "build/klyrdb",
        "Debug/klyrdb.exe",
        "Release/klyrdb.exe",
        "../Debug/klyrdb.exe",
        "../../Debug/klyrdb.exe",
        "../../../Debug/klyrdb.exe",
        "../Release/klyrdb.exe",
        "../../Release/klyrdb.exe",
        "klyrdb.exe",
        "klyrdb"
    };

    for (const auto& c : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(c, ec)) {
            return std::filesystem::absolute(c).string();
        }
    }
    return "klyrdb";
}

static int run_cli(const std::string& exe, const std::string& args) {
#ifdef _WIN32
    std::string cmd = "\"\"" + exe + "\" " + args + " > nul 2>&1\"";
#else
    std::string cmd = "\"" + exe + "\" " + args + " > /dev/null 2>&1";
#endif
    return std::system(cmd.c_str());
}

static void test_cli_version_and_help() {
    std::string exe = find_klyrdb_executable();

    EXPECT_EQ(run_cli(exe, "version"), 0);
    EXPECT_EQ(run_cli(exe, "--version"), 0);
    EXPECT_EQ(run_cli(exe, "-v"), 0);
    EXPECT_EQ(run_cli(exe, "help"), 0);
    EXPECT_EQ(run_cli(exe, "--help"), 0);
    EXPECT_EQ(run_cli(exe, "-h"), 0);

    // Invalid commands must fail with exit code 1
    EXPECT_NE(run_cli(exe, "nonexistent_command"), 0);
    EXPECT_NE(run_cli(exe, "info nonexistent_db_12345.kdb"), 0);
}

static void test_cli_database_and_sql_workflow() {
    std::string exe = find_klyrdb_executable();
    auto path = temp_test_file("cli_smoke_db.kdb");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    ScopedTempFile wal_cleanup(path.string() + ".wal");

    // 1. Create database
    EXPECT_EQ(run_cli(exe, "create " + path.string()), 0);
    EXPECT_EQ(run_cli(exe, "info " + path.string()), 0);

    // 2. DDL & DML
    EXPECT_EQ(run_cli(exe, "sql " + path.string() + " \"CREATE TABLE users (id INT PRIMARY KEY, name TEXT);\""), 0);
    EXPECT_EQ(run_cli(exe, "sql " + path.string() + " \"INSERT INTO users VALUES (1, 'Alice');\""), 0);
    EXPECT_EQ(run_cli(exe, "sql " + path.string() + " \"INSERT INTO users VALUES (2, 'Bob');\""), 0);
    EXPECT_EQ(run_cli(exe, "sql " + path.string() + " \"SELECT * FROM users;\""), 0);

    // 3. EXPLAIN & EXPLAIN ANALYZE
    EXPECT_EQ(run_cli(exe, "sql " + path.string() + " \"EXPLAIN SELECT * FROM users WHERE id = 1;\""), 0);
    EXPECT_EQ(run_cli(exe, "sql " + path.string() + " \"EXPLAIN ANALYZE SELECT * FROM users WHERE id = 1;\""), 0);

    // 4. Invalid SQL syntax error
    EXPECT_NE(run_cli(exe, "sql " + path.string() + " \"SELECT * FROM;\""), 0);

    // 5. Diagnostics, Statistics & Checkpointing
    EXPECT_EQ(run_cli(exe, "stats " + path.string()), 0);
    EXPECT_EQ(run_cli(exe, "stats-reset " + path.string()), 0);
    EXPECT_EQ(run_cli(exe, "diagnostics " + path.string() + " --json"), 0);
    EXPECT_EQ(run_cli(exe, "analyze " + path.string()), 0);
    EXPECT_EQ(run_cli(exe, "checkpoint " + path.string()), 0);
    EXPECT_EQ(run_cli(exe, "integrity-check " + path.string()), 0);
}

static void test_cli_backup_dump_csv_workflow() {
    std::string exe = find_klyrdb_executable();
    auto db_path = temp_test_file("cli_smoke_src.kdb");
    ScopedTempFile cleanup(db_path);
    ScopedTempFile lock_cleanup(db_path.string() + ".lock");
    ScopedTempFile wal_cleanup(db_path.string() + ".wal");

    EXPECT_EQ(run_cli(exe, "create " + db_path.string()), 0);
    EXPECT_EQ(run_cli(exe, "sql " + db_path.string() + " \"CREATE TABLE items (id INT PRIMARY KEY, val TEXT);\""), 0);
    EXPECT_EQ(run_cli(exe, "sql " + db_path.string() + " \"INSERT INTO items VALUES (10, 'widget');\""), 0);

    // 1. Physical Backup, Verify & Restore
    auto bkp_dir = temp_test_file("cli_smoke_backup_dir");
    std::filesystem::create_directories(bkp_dir);
    ScopedTempFile bkp_cleanup(bkp_dir);

    EXPECT_EQ(run_cli(exe, "backup " + db_path.string() + " " + bkp_dir.string() + " --overwrite"), 0);
    EXPECT_EQ(run_cli(exe, "backup-verify " + bkp_dir.string()), 0);

    auto restored_path = temp_test_file("cli_smoke_restored.kdb");
    ScopedTempFile r_cleanup(restored_path);
    ScopedTempFile r_lock(restored_path.string() + ".lock");
    ScopedTempFile r_wal(restored_path.string() + ".wal");

    EXPECT_EQ(run_cli(exe, "restore " + bkp_dir.string() + " " + restored_path.string() + " --force"), 0);
    EXPECT_EQ(run_cli(exe, "integrity-check " + restored_path.string()), 0);

    // 2. Logical Dump & Import
    auto dump_sql = temp_test_file("cli_smoke_dump.sql");
    ScopedTempFile dump_cleanup(dump_sql);

    EXPECT_EQ(run_cli(exe, "dump " + db_path.string() + " " + dump_sql.string()), 0);

    auto imported_path = temp_test_file("cli_smoke_imported.kdb");
    ScopedTempFile imp_cleanup(imported_path);
    ScopedTempFile imp_lock(imported_path.string() + ".lock");
    ScopedTempFile imp_wal(imported_path.string() + ".wal");

    EXPECT_EQ(run_cli(exe, "import " + dump_sql.string() + " " + imported_path.string() + " --replace"), 0);
    EXPECT_EQ(run_cli(exe, "integrity-check " + imported_path.string()), 0);

    // 3. CSV Export & Import
    auto export_csv = temp_test_file("cli_smoke_items.csv");
    ScopedTempFile csv_cleanup(export_csv);

    EXPECT_EQ(run_cli(exe, "export-csv " + db_path.string() + " items " + export_csv.string()), 0);
    EXPECT_EQ(run_cli(exe, "sql " + db_path.string() + " \"CREATE TABLE items_copy (id INT PRIMARY KEY, val TEXT);\""), 0);
    EXPECT_EQ(run_cli(exe, "import-csv " + export_csv.string() + " " + db_path.string() + " items_copy"), 0);
    EXPECT_EQ(run_cli(exe, "sql " + db_path.string() + " \"SELECT * FROM items_copy;\""), 0);
}

int main() {
    std::cout << "=== Running CLI Smoke & Subprocess Tests ===\n";

    run_test("test_cli_version_and_help", test_cli_version_and_help);
    run_test("test_cli_database_and_sql_workflow", test_cli_database_and_sql_workflow);
    run_test("test_cli_backup_dump_csv_workflow", test_cli_backup_dump_csv_workflow);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
