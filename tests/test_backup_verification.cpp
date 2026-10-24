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

static void test_valid_backup_verification() {
    auto db_path = temp_test_file("test_vfy_valid");
    ScopedTempFile db_cleanup(db_path);
    auto backup_dir = db_path.parent_path() / (db_path.filename().string() + "_bak");
    ScopedTempDir bak_cleanup(backup_dir);

    {
        auto pf = PageFile::create(db_path, false).value();
        BufferPoolManager bpm(*pf, 16);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        EXPECT_TRUE(executor.execute("CREATE TABLE accounts (id INTEGER PRIMARY KEY, balance INTEGER);").is_ok());
        for (int i = 1; i <= 20; ++i) {
            EXPECT_TRUE(executor.execute("INSERT INTO accounts VALUES (" + std::to_string(i) + ", 100);").is_ok());
        }
        (void)bpm.flush_all_pages();
        (void)pf->sync();
    }

    BackupManager mgr;
    auto bk_res = mgr.create_backup(db_path, backup_dir);
    EXPECT_TRUE(bk_res.is_ok());

    BackupVerifier verifier;
    auto vfy_res = verifier.verify(backup_dir);
    EXPECT_TRUE(vfy_res.is_ok());

    const auto& report = vfy_res.value();
    EXPECT_TRUE(report.is_valid);
    EXPECT_EQ(report.failure_count(), 0ULL);
    EXPECT_TRUE(report.passed_count() >= 5ULL);
}

static void test_verification_non_destructive() {
    auto db_path = temp_test_file("test_vfy_nondestr");
    ScopedTempFile db_cleanup(db_path);
    auto backup_dir = db_path.parent_path() / (db_path.filename().string() + "_bak");
    ScopedTempDir bak_cleanup(backup_dir);

    {
        auto pf = PageFile::create(db_path, false).value();
        BufferPoolManager bpm(*pf, 16);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);
        EXPECT_TRUE(executor.execute("CREATE TABLE t (id INTEGER PRIMARY KEY);").is_ok());
        (void)bpm.flush_all_pages();
        (void)pf->sync();
    }

    BackupManager mgr;
    EXPECT_TRUE(mgr.create_backup(db_path, backup_dir).is_ok());

    auto initial_db_crc = calculate_file_crc32(backup_dir / "data.kdb").value();
    auto initial_manifest_crc = calculate_file_crc32(backup_dir / "manifest.json").value();

    BackupVerifier verifier;
    // Run verification twice
    auto r1 = verifier.verify(backup_dir);
    EXPECT_TRUE(r1.is_ok() && r1.value().is_valid);
    auto r2 = verifier.verify(backup_dir);
    EXPECT_TRUE(r2.is_ok() && r2.value().is_valid);

    auto after_db_crc = calculate_file_crc32(backup_dir / "data.kdb").value();
    auto after_manifest_crc = calculate_file_crc32(backup_dir / "manifest.json").value();

    EXPECT_EQ(initial_db_crc, after_db_crc);
    EXPECT_EQ(initial_manifest_crc, after_manifest_crc);
}

int main() {
    std::cout << "=== Running test_backup_verification ===" << std::endl;
    run_test("test_valid_backup_verification", test_valid_backup_verification);
    run_test("test_verification_non_destructive", test_verification_non_destructive);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed." << std::endl;
    return g_failed_tests > 0 ? 1 : 0;
}
