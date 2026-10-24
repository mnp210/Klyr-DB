#include "test_framework.hpp"
#include "klyrdb/backup/backup_manager.hpp"
#include "klyrdb/backup/backup_verifier.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/sql/executor.hpp"
#include <fstream>

using namespace klyrdb;
using namespace klyrdb::testing;

static void setup_test_backup(const std::filesystem::path& db_path, const std::filesystem::path& backup_dir) {
    auto pf = PageFile::create(db_path, false).value();
    BufferPoolManager bpm(*pf, 16);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();
    SqlExecutor executor(bpm, rm, *tc, *ic);

    (void)executor.execute("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT);");
    (void)executor.execute("INSERT INTO items VALUES (1, 'Widget');");
    (void)bpm.flush_all_pages();
    (void)pf->sync();

    BackupManager mgr;
    BackupOptions opts;
    opts.overwrite = true;
    (void)mgr.create_backup(db_path, backup_dir, opts);
}

static void test_missing_manifest() {
    auto db_path = temp_test_file("test_corr_noman");
    ScopedTempFile db_cleanup(db_path);
    auto backup_dir = db_path.parent_path() / (db_path.filename().string() + "_bak");
    ScopedTempDir bak_cleanup(backup_dir);

    setup_test_backup(db_path, backup_dir);
    EXPECT_TRUE(std::filesystem::remove(backup_dir / "manifest.json"));

    BackupVerifier verifier;
    auto res = verifier.verify(backup_dir);
    EXPECT_TRUE(res.is_err() || !res.value().is_valid);
}

static void test_malformed_json_manifest() {
    auto db_path = temp_test_file("test_corr_badjson");
    ScopedTempFile db_cleanup(db_path);
    auto backup_dir = db_path.parent_path() / (db_path.filename().string() + "_bak");
    ScopedTempDir bak_cleanup(backup_dir);

    setup_test_backup(db_path, backup_dir);

    // Overwrite manifest with invalid JSON
    {
        std::ofstream ofs(backup_dir / "manifest.json", std::ios::trunc);
        ofs << "{ this is not valid JSON content !!! }}}";
    }

    BackupVerifier verifier;
    auto res = verifier.verify(backup_dir);
    EXPECT_TRUE(res.is_err() || !res.value().is_valid);
}

static void test_corrupted_database_bytes() {
    auto db_path = temp_test_file("test_corr_bytes");
    ScopedTempFile db_cleanup(db_path);
    auto backup_dir = db_path.parent_path() / (db_path.filename().string() + "_bak");
    ScopedTempDir bak_cleanup(backup_dir);

    setup_test_backup(db_path, backup_dir);

    // Flip bits in data.kdb
    {
        std::fstream fs(backup_dir / "data.kdb", std::ios::in | std::ios::out | std::ios::binary);
        fs.seekp(100);
        char b = 0;
        fs.read(&b, 1);
        b ^= 0xFF;
        fs.seekp(100);
        fs.write(&b, 1);
    }

    BackupVerifier verifier;
    auto res = verifier.verify(backup_dir);
    EXPECT_TRUE(res.is_err() || (res.is_ok() && !res.value().is_valid));
}

static void test_incompatible_format_version() {
    auto db_path = temp_test_file("test_corr_ver");
    ScopedTempFile db_cleanup(db_path);
    auto backup_dir = db_path.parent_path() / (db_path.filename().string() + "_bak");
    ScopedTempDir bak_cleanup(backup_dir);

    setup_test_backup(db_path, backup_dir);

    auto m = BackupManifest::read_from_file(backup_dir / "manifest.json").value();
    m.backup_format_version = 9999;
    EXPECT_TRUE(m.write_to_file(backup_dir / "manifest.json").is_ok());

    BackupVerifier verifier;
    auto res = verifier.verify(backup_dir);
    EXPECT_TRUE(res.is_err() || (res.is_ok() && !res.value().is_valid));
}

static void test_truncated_database_file() {
    auto db_path = temp_test_file("test_corr_trunc");
    ScopedTempFile db_cleanup(db_path);
    auto backup_dir = db_path.parent_path() / (db_path.filename().string() + "_bak");
    ScopedTempDir bak_cleanup(backup_dir);

    setup_test_backup(db_path, backup_dir);

    // Truncate data.kdb to half page (2048 bytes)
    std::filesystem::resize_file(backup_dir / "data.kdb", 2048);

    BackupVerifier verifier;
    auto res = verifier.verify(backup_dir);
    EXPECT_TRUE(res.is_err() || (res.is_ok() && !res.value().is_valid));
}

int main() {
    std::cout << "=== Running test_backup_corruption ===" << std::endl;
    run_test("test_missing_manifest", test_missing_manifest);
    run_test("test_malformed_json_manifest", test_malformed_json_manifest);
    run_test("test_corrupted_database_bytes", test_corrupted_database_bytes);
    run_test("test_incompatible_format_version", test_incompatible_format_version);
    run_test("test_truncated_database_file", test_truncated_database_file);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed." << std::endl;
    return g_failed_tests > 0 ? 1 : 0;
}
