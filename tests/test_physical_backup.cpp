#include "test_framework.hpp"
#include "klyrdb/backup/backup_manager.hpp"
#include "klyrdb/backup/backup_manifest.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/sql/executor.hpp"
#include "klyrdb/concurrency/lock_manager.hpp"
#include <chrono>
#include <atomic>
#include <thread>

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_empty_database_backup() {
    auto db_path = temp_test_file("test_bk_empty");
    ScopedTempFile db_cleanup(db_path);
    auto backup_dir = db_path.parent_path() / (db_path.filename().string() + "_bak");
    ScopedTempDir bak_cleanup(backup_dir);

    // Create empty database
    auto create_res = PageFile::create(db_path, false);
    EXPECT_TRUE(create_res.is_ok());

    BackupManager mgr;
    BackupOptions opts;
    auto res = mgr.create_backup(db_path, backup_dir, opts);
    EXPECT_TRUE(res.is_ok());

    const auto& bres = res.value();
    EXPECT_EQ(bres.backup_path, backup_dir);
    EXPECT_TRUE(bres.duration_ms >= 0.0);
    EXPECT_TRUE(std::filesystem::exists(backup_dir / "manifest.json"));
    EXPECT_TRUE(std::filesystem::exists(backup_dir / "data.kdb"));

    auto manifest_res = BackupManifest::read_from_file(backup_dir / "manifest.json");
    EXPECT_TRUE(manifest_res.is_ok());
    const auto& manifest = manifest_res.value();
    EXPECT_EQ(manifest.backup_format_version, BackupManifest::CURRENT_BACKUP_FORMAT_VERSION);
    EXPECT_EQ(manifest.storage_format_version, BackupManifest::CURRENT_STORAGE_FORMAT_VERSION);
    EXPECT_EQ(manifest.page_count, 1U);
    EXPECT_EQ(manifest.checksum_algo, "CRC32");

    const auto* db_comp = manifest.find_component("database");
    EXPECT_TRUE(db_comp != nullptr);
    EXPECT_EQ(db_comp->size_bytes, 4096ULL);
    EXPECT_TRUE(db_comp->crc32 != 0U);
}

static void test_multi_table_database_backup() {
    auto db_path = temp_test_file("test_bk_multi");
    ScopedTempFile db_cleanup(db_path);
    std::filesystem::path wal_path = db_path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);
    auto backup_dir = db_path.parent_path() / (db_path.filename().string() + "_bak");
    ScopedTempDir bak_cleanup(backup_dir);

    {
        auto pf = PageFile::create(db_path, false).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        bpm.set_wal(wal.get());
        TransactionManager tm(bpm, *wal);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();

        SqlExecutor executor(bpm, rm, *tc, *ic);
        executor.set_transaction_manager(&tm);

        EXPECT_TRUE(executor.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);").is_ok());
        EXPECT_TRUE(executor.execute("CREATE INDEX idx_name ON users(name);").is_ok());
        EXPECT_TRUE(executor.execute("CREATE TABLE orders (id INTEGER PRIMARY KEY, user_id INTEGER, amount INTEGER);").is_ok());

        for (int i = 1; i <= 25; ++i) {
            std::string q = "INSERT INTO users VALUES (" + std::to_string(i) + ", 'Alice_" + std::to_string(i) + "');";
            EXPECT_TRUE(executor.execute(q).is_ok());
        }

        for (int i = 1; i <= 50; ++i) {
            std::string q = "INSERT INTO orders VALUES (" + std::to_string(i) + ", " + std::to_string((i % 25) + 1) + ", 99);";
            EXPECT_TRUE(executor.execute(q).is_ok());
        }

        (void)bpm.flush_all_pages();
        (void)pf->sync();
    }

    BackupManager mgr;
    BackupOptions opts;
    auto res = mgr.create_backup(db_path, backup_dir, opts);
    EXPECT_TRUE(res.is_ok());

    EXPECT_TRUE(std::filesystem::exists(backup_dir / "manifest.json"));
    EXPECT_TRUE(std::filesystem::exists(backup_dir / "data.kdb"));

    auto manifest_res = BackupManifest::read_from_file(backup_dir / "manifest.json");
    EXPECT_TRUE(manifest_res.is_ok());
    const auto& manifest = manifest_res.value();
    EXPECT_TRUE(manifest.page_count > 1U);
}

static void test_active_lock_rejection() {
    auto db_path = temp_test_file("test_bk_lock");
    ScopedTempFile db_cleanup(db_path);
    auto backup_dir = db_path.parent_path() / (db_path.filename().string() + "_bak");
    ScopedTempDir bak_cleanup(backup_dir);

    auto create_res = PageFile::create(db_path, false);
    EXPECT_TRUE(create_res.is_ok());

    // Acquire exclusive database lock on a separate thread to test cross-thread contention
    std::atomic<bool> thread_locked{false};
    std::atomic<bool> should_release{false};
    std::thread lock_thread([&]() {
        auto lock = LockManager::instance().get_lock(db_path, false);
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

    BackupManager mgr;
    BackupOptions opts;
    opts.lock_timeout = std::chrono::milliseconds(100);
    auto res = mgr.create_backup(db_path, backup_dir, opts);
    EXPECT_TRUE(res.is_err());
    EXPECT_EQ(res.status().code(), StatusCode::LockTimeout);

    should_release = true;
    lock_thread.join();
}

static void test_overwrite_protection() {
    auto db_path = temp_test_file("test_bk_overwrite");
    ScopedTempFile db_cleanup(db_path);
    auto backup_dir = db_path.parent_path() / (db_path.filename().string() + "_bak");
    ScopedTempDir bak_cleanup(backup_dir);

    auto create_res = PageFile::create(db_path, false);
    EXPECT_TRUE(create_res.is_ok());

    BackupManager mgr;
    BackupOptions opts;
    opts.overwrite = false;
    auto res1 = mgr.create_backup(db_path, backup_dir, opts);
    EXPECT_TRUE(res1.is_ok());

    // Second backup without overwrite should fail
    auto res2 = mgr.create_backup(db_path, backup_dir, opts);
    EXPECT_TRUE(res2.is_err());
    EXPECT_EQ(res2.status().code(), StatusCode::FileAlreadyExists);

    // Second backup with overwrite should succeed
    opts.overwrite = true;
    auto res3 = mgr.create_backup(db_path, backup_dir, opts);
    EXPECT_TRUE(res3.is_ok());
}

int main() {
    std::cout << "=== Running test_physical_backup ===" << std::endl;
    run_test("test_empty_database_backup", test_empty_database_backup);
    run_test("test_multi_table_database_backup", test_multi_table_database_backup);
    run_test("test_active_lock_rejection", test_active_lock_rejection);
    run_test("test_overwrite_protection", test_overwrite_protection);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed." << std::endl;
    return g_failed_tests > 0 ? 1 : 0;
}
