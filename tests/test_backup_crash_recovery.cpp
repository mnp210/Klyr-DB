#include "test_framework.hpp"
#include "klyrdb/backup/backup_manager.hpp"
#include "klyrdb/backup/restore_manager.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/storage/integrity_checker.hpp"
#include "klyrdb/sql/executor.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_backup_with_uncheckpointed_wal() {
    auto src_db = temp_test_file("test_bk_rec_src");
    ScopedTempFile src_cleanup(src_db);
    std::filesystem::path src_wal = src_db.string() + ".wal";
    ScopedTempFile src_wal_cleanup(src_wal);

    auto backup_dir = src_db.parent_path() / (src_db.filename().string() + "_bak");
    ScopedTempDir bak_cleanup(backup_dir);
    auto dest_db = temp_test_file("test_bk_rec_dest");
    ScopedTempFile dest_cleanup(dest_db);
    std::filesystem::path dest_wal = dest_db.string() + ".wal";
    ScopedTempFile dest_wal_cleanup(dest_wal);

    {
        auto pf = PageFile::create(src_db, false).value();
        auto wal = WriteAheadLog::open_or_create(src_wal).value();
        BufferPoolManager bpm(*pf, 16);
        bpm.set_wal(wal.get());
        TransactionManager tm(bpm, *wal);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);
        executor.set_transaction_manager(&tm);

        EXPECT_TRUE(executor.execute("CREATE TABLE ledger (id INTEGER PRIMARY KEY, val INTEGER);").is_ok());

        // Committed transactions
        EXPECT_TRUE(executor.execute("BEGIN TRANSACTION;").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO ledger VALUES (1, 100);").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO ledger VALUES (2, 200);").is_ok());
        EXPECT_TRUE(executor.execute("COMMIT;").is_ok());

        // Uncommitted transaction left active without COMMIT
        EXPECT_TRUE(executor.execute("BEGIN TRANSACTION;").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO ledger VALUES (3, 300);").is_ok());

        // Durably flush WAL
        (void)wal->flush();
    }

    // Perform physical backup
    BackupManager bk_mgr;
    BackupOptions bk_opts;
    auto bk_res = bk_mgr.create_backup(src_db, backup_dir, bk_opts);
    EXPECT_TRUE(bk_res.is_ok());

    // Restore with automatic recovery enabled
    RestoreManager rst_mgr;
    RestoreOptions rst_opts;
    rst_opts.run_recovery = true;
    auto rst_res = rst_mgr.restore(backup_dir, dest_db, rst_opts);
    EXPECT_TRUE(rst_res.is_ok());
    EXPECT_TRUE(rst_res.is_ok());

    // Verify restored state: records 1 and 2 exist, uncommitted record 3 rolled back
    {
        auto pf = PageFile::open(dest_db, false).value();
        BufferPoolManager bpm(*pf, 16);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        auto r1 = executor.execute("SELECT * FROM ledger WHERE id = 1;");
        EXPECT_TRUE(r1.is_ok());
        EXPECT_EQ(r1.value().row_count(), 1ULL);

        auto r2 = executor.execute("SELECT * FROM ledger WHERE id = 2;");
        EXPECT_TRUE(r2.is_ok());
        EXPECT_EQ(r2.value().row_count(), 1ULL);

        auto r3 = executor.execute("SELECT * FROM ledger WHERE id = 3;");
        EXPECT_TRUE(r3.is_ok());
        EXPECT_TRUE(r3.value().rows().empty());

        // Run integrity checker
        IntegrityChecker checker(*pf, nullptr);
        auto rep_res = checker.check_all();
        EXPECT_TRUE(rep_res.is_ok());
        EXPECT_TRUE(rep_res.value().is_healthy);
    }
}

int main() {
    std::cout << "=== Running test_backup_crash_recovery ===" << std::endl;
    run_test("test_backup_with_uncheckpointed_wal", test_backup_with_uncheckpointed_wal);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed." << std::endl;
    return g_failed_tests > 0 ? 1 : 0;
}
