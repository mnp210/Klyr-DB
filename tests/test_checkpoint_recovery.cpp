#include "test_framework.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/recovery/checkpoint_manager.hpp"
#include "klyrdb/recovery/recovery_manager.hpp"
#include "klyrdb/sql/executor.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_checkpoint_accelerated_recovery() {
    auto path = temp_test_file("test_chk_recov_accel");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);

    // 1. Initial workload and checkpoint
    {
        auto pf = PageFile::create(path, false).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        TransactionManager tm(bpm, *wal);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();

        SqlExecutor executor(bpm, rm, *tc, *ic);
        executor.set_transaction_manager(&tm);

        EXPECT_TRUE(executor.execute("CREATE TABLE orders (id INTEGER PRIMARY KEY, qty INTEGER);").is_ok());

        for (int i = 1; i <= 30; ++i) {
            std::string sql = "INSERT INTO orders VALUES (" + std::to_string(i) + ", " + std::to_string(i * 10) + ");";
            EXPECT_TRUE(executor.execute(sql).is_ok());
        }

        // Checkpoint establishing boundary
        CheckpointManager chk_mgr(bpm, *wal, tm);
        auto chk_res = chk_mgr.checkpoint(CheckpointMode::Safe);
        EXPECT_TRUE(chk_res.is_ok());

        // 2. Additional committed operations after checkpoint
        for (int i = 31; i <= 50; ++i) {
            std::string sql = "INSERT INTO orders VALUES (" + std::to_string(i) + ", " + std::to_string(i * 10) + ");";
            EXPECT_TRUE(executor.execute(sql).is_ok());
        }

        // 3. Uncommitted transaction
        EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO orders VALUES (999, 9990);").is_ok());

        // Flush dirty pages to disk so uncommitted modification is persisted on disk,
        // then drop frames to test that the recovery Undo pass correctly restores the before-image!
        EXPECT_TRUE(bpm.flush_all_pages().is_ok());
        bpm.drop_frames_without_flush();
    }

    // 4. Open and run recovery
    {
        auto pf = PageFile::open(path, false).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);

        RecoveryManager rm(bpm, *wal);
        EXPECT_TRUE(rm.needs_recovery());

        auto rec_res = rm.recover();
        EXPECT_TRUE(rec_res.is_ok());

        const auto& stats = rec_res.value();
        EXPECT_TRUE(stats.checkpoint_lsn > 0);
        EXPECT_EQ(stats.uncommitted_txns, 1ULL);
        EXPECT_TRUE(stats.pages_undone >= 1);
        EXPECT_TRUE(stats.records_scanned <= stats.total_log_records);

        // 5. Repeated recovery must be idempotent
        auto repeat_res = rm.recover();
        EXPECT_TRUE(repeat_res.is_ok());
        EXPECT_EQ(repeat_res.value().pages_redone, 0ULL);
        EXPECT_EQ(repeat_res.value().pages_undone, 0ULL);

        // 6. Verify table contents
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rec_mgr(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rec_mgr, *tc, *ic);

        auto sel = executor.execute("SELECT id FROM orders;");
        EXPECT_TRUE(sel.is_ok());
        EXPECT_EQ(sel.value().row_count(), 50ULL);

        auto check_uncommitted = executor.execute("SELECT id FROM orders WHERE id = 999;");
        EXPECT_TRUE(check_uncommitted.is_ok());
        EXPECT_EQ(check_uncommitted.value().row_count(), 0ULL);
    }
}

int main() {
    std::cout << "=== Running Checkpoint Recovery Tests ===\n";
    run_test("test_checkpoint_accelerated_recovery", test_checkpoint_accelerated_recovery);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
