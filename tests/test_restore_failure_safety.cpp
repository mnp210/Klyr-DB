#include "test_framework.hpp"
#include "klyrdb/backup/backup_manager.hpp"
#include "klyrdb/backup/restore_manager.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/sql/executor.hpp"
#include "klyrdb/concurrency/lock_manager.hpp"
#include <fstream>
#include <atomic>
#include <thread>

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_restore_existing_destination_no_force() {
    auto src_db = temp_test_file("test_rst_exist_src");
    ScopedTempFile src_cleanup(src_db);
    auto backup_dir = src_db.parent_path() / (src_db.filename().string() + "_bak");
    ScopedTempDir bak_cleanup(backup_dir);
    auto dest_db = temp_test_file("test_rst_exist_dest");
    ScopedTempFile dest_cleanup(dest_db);

    EXPECT_TRUE(PageFile::create(src_db, false).is_ok());
    BackupManager bk_mgr;
    EXPECT_TRUE(bk_mgr.create_backup(src_db, backup_dir).is_ok());

    // Create existing target file
    EXPECT_TRUE(PageFile::create(dest_db, false).is_ok());

    RestoreManager rst_mgr;
    RestoreOptions opts;
    opts.force_overwrite = false;
    auto res = rst_mgr.restore(backup_dir, dest_db, opts);
    EXPECT_TRUE(res.is_err());
    EXPECT_EQ(res.status().code(), StatusCode::FileAlreadyExists);
}

static void test_restore_locked_destination() {
    auto src_db = temp_test_file("test_rst_lock_src");
    ScopedTempFile src_cleanup(src_db);
    auto backup_dir = src_db.parent_path() / (src_db.filename().string() + "_bak");
    ScopedTempDir bak_cleanup(backup_dir);
    auto dest_db = temp_test_file("test_rst_lock_dest");
    ScopedTempFile dest_cleanup(dest_db);

    EXPECT_TRUE(PageFile::create(src_db, false).is_ok());
    BackupManager bk_mgr;
    EXPECT_TRUE(bk_mgr.create_backup(src_db, backup_dir).is_ok());

    EXPECT_TRUE(PageFile::create(dest_db, false).is_ok());

    // Lock destination on another thread to test contention
    std::atomic<bool> thread_locked{false};
    std::atomic<bool> should_release{false};
    std::thread lock_thread([&]() {
        auto lock = LockManager::instance().get_lock(dest_db, false);
        EXPECT_TRUE(lock->acquire_exclusive(std::chrono::milliseconds(2000)).is_ok());
        thread_locked = true;
        while (!should_release) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        (void)lock->release_exclusive();
    });

    while (!thread_locked) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    RestoreManager rst_mgr;
    RestoreOptions opts;
    opts.force_overwrite = true;
    opts.lock_timeout = std::chrono::milliseconds(100);
    auto res = rst_mgr.restore(backup_dir, dest_db, opts);
    EXPECT_TRUE(res.is_err());
    EXPECT_EQ(res.status().code(), StatusCode::LockTimeout);

    should_release = true;
    lock_thread.join();
}

static void test_restore_corrupted_backup_safeguard() {
    auto src_db = temp_test_file("test_rst_corr_src");
    ScopedTempFile src_cleanup(src_db);
    auto backup_dir = src_db.parent_path() / (src_db.filename().string() + "_bak");
    ScopedTempDir bak_cleanup(backup_dir);
    auto dest_db = temp_test_file("test_rst_corr_dest");
    ScopedTempFile dest_cleanup(dest_db);

    EXPECT_TRUE(PageFile::create(src_db, false).is_ok());
    BackupManager bk_mgr;
    EXPECT_TRUE(bk_mgr.create_backup(src_db, backup_dir).is_ok());

    // Corrupt backup manifest
    {
        std::ofstream ofs(backup_dir / "manifest.json", std::ios::trunc);
        ofs << "corrupt data";
    }

    // Set up destination with specific initial content
    {
        auto pf = PageFile::create(dest_db, false).value();
        BufferPoolManager bpm(*pf, 16);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);
        EXPECT_TRUE(executor.execute("CREATE TABLE original (v TEXT);").is_ok());
        (void)bpm.flush_all_pages();
        (void)pf->sync();
    }

    auto orig_crc = calculate_file_crc32(dest_db).value();

    RestoreManager rst_mgr;
    RestoreOptions opts;
    opts.force_overwrite = true;
    auto res = rst_mgr.restore(backup_dir, dest_db, opts);
    EXPECT_TRUE(res.is_err());

    // Destination should be completely untouched
    auto after_crc = calculate_file_crc32(dest_db).value();
    EXPECT_EQ(orig_crc, after_crc);
}

int main() {
    std::cout << "=== Running test_restore_failure_safety ===" << std::endl;
    run_test("test_restore_existing_destination_no_force", test_restore_existing_destination_no_force);
    run_test("test_restore_locked_destination", test_restore_locked_destination);
    run_test("test_restore_corrupted_backup_safeguard", test_restore_corrupted_backup_safeguard);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed." << std::endl;
    return g_failed_tests > 0 ? 1 : 0;
}
