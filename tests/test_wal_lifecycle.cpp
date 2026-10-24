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
#include "klyrdb/recovery/crash_simulator.hpp"
#include "klyrdb/sql/executor.hpp"
#include <fstream>

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_crash_before_checkpoint() {
    auto path = temp_test_file("test_crash_before_chk");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);

    // Setup and insert committed row
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

        EXPECT_TRUE(executor.execute("CREATE TABLE t (id INTEGER PRIMARY KEY);").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO t VALUES (1);").is_ok());

        CrashSimulator::arm(CrashPoint::BeforeCheckpoint);
        CheckpointManager chk_mgr(bpm, *wal, tm);
        try {
            (void)chk_mgr.checkpoint(CheckpointMode::Safe);
        } catch (const CrashSimulationException&) {
            // Simulated crash
        }
        CrashSimulator::disarm();
        bpm.drop_frames_without_flush();
    }

    // Recover and verify
    {
        auto pf = PageFile::open(path, false).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        RecoveryManager rm(bpm, *wal);
        EXPECT_TRUE(rm.recover().is_ok());

        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rec_mgr(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rec_mgr, *tc, *ic);

        auto sel = executor.execute("SELECT id FROM t;");
        EXPECT_TRUE(sel.is_ok());
        EXPECT_EQ(sel.value().row_count(), 1ULL);
    }
}

static void test_crash_after_pages_flushed_before_metadata() {
    auto path = temp_test_file("test_crash_after_pages_flushed");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);

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

        EXPECT_TRUE(executor.execute("CREATE TABLE t2 (id INTEGER PRIMARY KEY, v TEXT);").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO t2 VALUES (42, 'durable_value');").is_ok());

        CrashSimulator::arm(CrashPoint::AfterPagesFlushedBeforeCheckpointDurable);
        CheckpointManager chk_mgr(bpm, *wal, tm);
        try {
            (void)chk_mgr.checkpoint(CheckpointMode::Safe);
        } catch (const CrashSimulationException&) {
            // Simulated crash
        }
        CrashSimulator::disarm();
        bpm.drop_frames_without_flush();
    }

    {
        auto pf = PageFile::open(path, false).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        RecoveryManager rm(bpm, *wal);
        EXPECT_TRUE(rm.recover().is_ok());

        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rec_mgr(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rec_mgr, *tc, *ic);

        auto sel = executor.execute("SELECT v FROM t2 WHERE id = 42;");
        EXPECT_TRUE(sel.is_ok());
        EXPECT_EQ(sel.value().row_count(), 1ULL);
        EXPECT_EQ(sel.value().rows()[0].get(0).as_text(), "durable_value");
    }
}

static void test_crash_during_wal_truncation() {
    auto path = temp_test_file("test_crash_truncation");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);

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

        EXPECT_TRUE(executor.execute("CREATE TABLE t3 (id INTEGER PRIMARY KEY);").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO t3 VALUES (100);").is_ok());

        CrashSimulator::arm(CrashPoint::DuringWalTruncation);
        CheckpointManager chk_mgr(bpm, *wal, tm);
        try {
            (void)chk_mgr.checkpoint(CheckpointMode::Safe);
        } catch (const CrashSimulationException&) {
            // Simulated crash
        }
        CrashSimulator::disarm();
        bpm.drop_frames_without_flush();
    }

    // Because atomic rename wasn't reached, original WAL is still intact!
    {
        auto pf = PageFile::open(path, false).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        RecoveryManager rm(bpm, *wal);
        EXPECT_TRUE(rm.recover().is_ok());

        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rec_mgr(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rec_mgr, *tc, *ic);

        auto sel = executor.execute("SELECT id FROM t3;");
        EXPECT_TRUE(sel.is_ok());
        EXPECT_EQ(sel.value().row_count(), 1ULL);
    }
}

static void test_trailing_incomplete_wal_record() {
    auto path = temp_test_file("test_trailing_incomplete");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);

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

        EXPECT_TRUE(executor.execute("CREATE TABLE t4 (id INTEGER PRIMARY KEY);").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO t4 VALUES (1);").is_ok());
        EXPECT_TRUE(wal->flush().is_ok());
    }

    // Append 15 bytes of half-written trailing garbage to the WAL
    {
        std::ofstream ws(wal_path, std::ios::binary | std::ios::app);
        char partial[15] = {0x57, 0x41, 0x4C, 0x52}; // WALR magic but incomplete header
        ws.write(partial, 15);
    }

    // Recovery must safely ignore incomplete trailing record
    {
        auto pf = PageFile::open(path, false).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        RecoveryManager rm(bpm, *wal);
        EXPECT_TRUE(rm.recover().is_ok());

        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rec_mgr(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rec_mgr, *tc, *ic);

        auto sel = executor.execute("SELECT id FROM t4;");
        EXPECT_TRUE(sel.is_ok());
        EXPECT_EQ(sel.value().row_count(), 1ULL);
    }
}

int main() {
    std::cout << "=== Running WAL Lifecycle & Crash Tests ===\n";
    run_test("test_crash_before_checkpoint", test_crash_before_checkpoint);
    run_test("test_crash_after_pages_flushed_before_metadata", test_crash_after_pages_flushed_before_metadata);
    run_test("test_crash_during_wal_truncation", test_crash_during_wal_truncation);
    run_test("test_trailing_incomplete_wal_record", test_trailing_incomplete_wal_record);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
