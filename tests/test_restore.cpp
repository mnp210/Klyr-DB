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
#include "klyrdb/recovery/checkpoint_manager.hpp"
#include "klyrdb/sql/executor.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_full_restore_fresh_destination() {
    auto src_db_path = temp_test_file("test_rst_src");
    ScopedTempFile src_cleanup(src_db_path);
    auto backup_dir = src_db_path.parent_path() / (src_db_path.filename().string() + "_bak");
    ScopedTempDir bak_cleanup(backup_dir);
    auto dest_db_path = temp_test_file("test_rst_dest");
    ScopedTempFile dest_cleanup(dest_db_path);
    std::filesystem::path dest_wal = dest_db_path.string() + ".wal";
    ScopedTempFile dest_wal_cleanup(dest_wal);

    // 1. Setup source DB
    {
        auto pf = PageFile::create(src_db_path, false).value();
        BufferPoolManager bpm(*pf, 16);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        EXPECT_TRUE(executor.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);").is_ok());
        EXPECT_TRUE(executor.execute("CREATE INDEX idx_users_name ON users(name);").is_ok());
        for (int i = 1; i <= 30; ++i) {
            std::string q = "INSERT INTO users VALUES (" + std::to_string(i) + ", 'Person_" + std::to_string(i) + "');";
            EXPECT_TRUE(executor.execute(q).is_ok());
        }
        (void)bpm.flush_all_pages();
        (void)pf->sync();
    }

    // 2. Backup
    BackupManager bk_mgr;
    EXPECT_TRUE(bk_mgr.create_backup(src_db_path, backup_dir).is_ok());

    // 3. Restore
    RestoreManager rst_mgr;
    RestoreOptions rst_opts;
    auto rst_res = rst_mgr.restore(backup_dir, dest_db_path, rst_opts);
    EXPECT_TRUE(rst_res.is_ok());
    EXPECT_TRUE(rst_res.value().pages_restored > 0);

    // 4. Validate restored DB operations
    {
        auto pf = PageFile::open(dest_db_path, false).value();
        auto wal = WriteAheadLog::open_or_create(dest_wal).value();
        BufferPoolManager bpm(*pf, 16);
        bpm.set_wal(wal.get());
        TransactionManager tm(bpm, *wal);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);
        executor.set_transaction_manager(&tm);

        // Point lookup
        auto sel_res = executor.execute("SELECT * FROM users WHERE id = 15;");
        EXPECT_TRUE(sel_res.is_ok());
        EXPECT_EQ(sel_res.value().row_count(), 1ULL);
        EXPECT_EQ(sel_res.value().rows()[0].get(1).as_text(), "Person_15");

        // Index scan
        auto idx_res = executor.execute("SELECT * FROM users WHERE name = 'Person_20';");
        EXPECT_TRUE(idx_res.is_ok());
        EXPECT_EQ(idx_res.value().row_count(), 1ULL);
        EXPECT_EQ(idx_res.value().rows()[0].get(0).as_integer(), 20LL);

        // INSERT
        EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (99, 'NewGuy');").is_ok());

        // UPDATE
        EXPECT_TRUE(executor.execute("UPDATE users SET name = 'Modified' WHERE id = 1;").is_ok());

        // DELETE
        EXPECT_TRUE(executor.execute("DELETE FROM users WHERE id = 2;").is_ok());

        // Verify changes
        auto mod_res = executor.execute("SELECT name FROM users WHERE id = 1;");
        EXPECT_TRUE(mod_res.is_ok());
        EXPECT_EQ(mod_res.value().rows()[0].get(0).as_text(), "Modified");
        auto del_res = executor.execute("SELECT * FROM users WHERE id = 2;");
        EXPECT_TRUE(del_res.is_ok());
        EXPECT_TRUE(del_res.value().rows().empty());

        // Checkpoint on restored database
        CheckpointManager chk_mgr(bpm, *wal, tm);
        auto chk_res = chk_mgr.checkpoint(CheckpointMode::Safe);
        EXPECT_TRUE(chk_res.is_ok());
    }
}

int main() {
    std::cout << "=== Running test_restore ===" << std::endl;
    run_test("test_full_restore_fresh_destination", test_full_restore_fresh_destination);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed." << std::endl;
    return g_failed_tests > 0 ? 1 : 0;
}
