#include "test_framework.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/recovery/recovery_manager.hpp"
#include "klyrdb/recovery/crash_simulator.hpp"
#include "klyrdb/sql/executor.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

// Helper to set up a populated baseline database
static void setup_test_db(const std::filesystem::path& path) {
    auto pf = PageFile::create(path).value();
    std::filesystem::path wal_path = path.string() + ".wal";
    auto wal = WriteAheadLog::open_or_create(wal_path).value();
    BufferPoolManager bpm(*pf, 32);
    TransactionManager tm(bpm, *wal);

    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();
    SqlExecutor executor(bpm, rm, *tc, *ic);
    executor.set_transaction_manager(&tm);

    EXPECT_TRUE(executor.execute("CREATE TABLE users (id INTEGER, name TEXT, score INTEGER);").is_ok());
    EXPECT_TRUE(executor.execute("CREATE INDEX idx_user_id ON users (id);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (1, 'Alice', 100);").is_ok());
    (void)bpm.flush_all_pages();
}

// 1. Crash point: BeforePageModify
static void test_crash_before_page_modify() {
    auto path = temp_test_file("crash_pt1");
    ScopedTempFile cleanup(path);
    setup_test_db(path);

    bool caught = false;
    {
        auto pf = PageFile::open(path).value();
        std::filesystem::path wal_path = path.string() + ".wal";
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        TransactionManager tm(bpm, *wal);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);
        executor.set_transaction_manager(&tm);

        EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
        CrashSimulator::set_target(CrashPoint::BeforePageModify);

        try {
            (void)executor.execute("INSERT INTO users VALUES (2, 'Bob', 200);");
        } catch (const CrashSimulationException&) {
            caught = true;
            bpm.drop_frames_without_flush();
        }
    }
    EXPECT_TRUE(caught);

    // Reopen & Recover
    {
        auto pf = PageFile::open(path).value();
        std::filesystem::path wal_path = path.string() + ".wal";
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        RecoveryManager rec_mgr(bpm, *wal);
        (void)rec_mgr.recover();

        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        auto sel = executor.execute("SELECT * FROM users;");
        EXPECT_TRUE(sel.is_ok());
        EXPECT_EQ(sel.value().row_count(), 1ULL); // Only Alice
    }
}

// 2. Crash point: AfterLogBeforePageFlush
static void test_crash_after_log_before_page_flush() {
    auto path = temp_test_file("crash_pt2");
    ScopedTempFile cleanup(path);
    setup_test_db(path);

    bool caught = false;
    {
        auto pf = PageFile::open(path).value();
        std::filesystem::path wal_path = path.string() + ".wal";
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        TransactionManager tm(bpm, *wal);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);
        executor.set_transaction_manager(&tm);

        EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
        CrashSimulator::set_target(CrashPoint::AfterLogBeforePageFlush);

        try {
            (void)executor.execute("INSERT INTO users VALUES (2, 'Bob', 200);");
        } catch (const CrashSimulationException&) {
            caught = true;
            bpm.drop_frames_without_flush();
        }
    }
    EXPECT_TRUE(caught);

    // Recover
    {
        auto pf = PageFile::open(path).value();
        std::filesystem::path wal_path = path.string() + ".wal";
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        RecoveryManager rec_mgr(bpm, *wal);
        (void)rec_mgr.recover();

        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        auto sel = executor.execute("SELECT * FROM users;");
        EXPECT_TRUE(sel.is_ok());
        EXPECT_EQ(sel.value().row_count(), 1ULL);
    }
}

// 3. Crash point: AfterMemoryModify
static void test_crash_after_memory_modify() {
    auto path = temp_test_file("crash_pt3");
    ScopedTempFile cleanup(path);
    setup_test_db(path);

    bool caught = false;
    {
        auto pf = PageFile::open(path).value();
        std::filesystem::path wal_path = path.string() + ".wal";
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        TransactionManager tm(bpm, *wal);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);
        executor.set_transaction_manager(&tm);

        EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
        CrashSimulator::set_target(CrashPoint::AfterMemoryModify);

        try {
            (void)executor.execute("UPDATE users SET score = 999 WHERE id = 1;");
        } catch (const CrashSimulationException&) {
            caught = true;
            bpm.drop_frames_without_flush();
        }
    }
    EXPECT_TRUE(caught);

    // Recover: score must still be 100
    {
        auto pf = PageFile::open(path).value();
        std::filesystem::path wal_path = path.string() + ".wal";
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        RecoveryManager rec_mgr(bpm, *wal);
        (void)rec_mgr.recover();

        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        auto sel = executor.execute("SELECT score FROM users WHERE id = 1;");
        EXPECT_TRUE(sel.is_ok());
        EXPECT_EQ(sel.value().rows()[0].get(0).as_integer(), 100LL);
    }
}

// 4. Crash point: DuringWalWrite
static void test_crash_during_wal_write() {
    auto path = temp_test_file("crash_pt4");
    ScopedTempFile cleanup(path);
    setup_test_db(path);

    bool caught = false;
    {
        auto pf = PageFile::open(path).value();
        std::filesystem::path wal_path = path.string() + ".wal";
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        TransactionManager tm(bpm, *wal);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);
        executor.set_transaction_manager(&tm);

        EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
        CrashSimulator::set_target(CrashPoint::DuringWalWrite);

        try {
            (void)executor.execute("INSERT INTO users VALUES (2, 'Bob', 200);");
        } catch (const CrashSimulationException&) {
            caught = true;
            bpm.drop_frames_without_flush();
        }
    }
    EXPECT_TRUE(caught);

    // Recover: WAL has incomplete truncated tail, which must be ignored gracefully
    {
        auto pf = PageFile::open(path).value();
        std::filesystem::path wal_path = path.string() + ".wal";
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        RecoveryManager rec_mgr(bpm, *wal);
        auto rec_res = rec_mgr.recover();
        EXPECT_TRUE(rec_res.is_ok());

        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        auto sel = executor.execute("SELECT * FROM users;");
        EXPECT_TRUE(sel.is_ok());
        EXPECT_EQ(sel.value().row_count(), 1ULL);
    }
}

// 5. Crash point: DuringPageFlush
static void test_crash_during_page_flush() {
    auto path = temp_test_file("crash_pt5");
    ScopedTempFile cleanup(path);
    setup_test_db(path);

    bool caught = false;
    {
        auto pf = PageFile::open(path).value();
        std::filesystem::path wal_path = path.string() + ".wal";
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        TransactionManager tm(bpm, *wal);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);
        executor.set_transaction_manager(&tm);

        EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (2, 'Bob', 200);").is_ok());

        CrashSimulator::set_target(CrashPoint::DuringPageFlush);
        try {
            (void)bpm.flush_all_pages();
        } catch (const CrashSimulationException&) {
            caught = true;
            bpm.drop_frames_without_flush();
        }
    }
    EXPECT_TRUE(caught);

    // Recover: Bob was uncommitted, so undo ensures only Alice remains
    {
        auto pf = PageFile::open(path).value();
        std::filesystem::path wal_path = path.string() + ".wal";
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        RecoveryManager rec_mgr(bpm, *wal);
        EXPECT_TRUE(rec_mgr.recover().is_ok());

        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        auto sel = executor.execute("SELECT * FROM users;");
        EXPECT_TRUE(sel.is_ok());
        EXPECT_EQ(sel.value().row_count(), 1ULL);
    }
}

// 6. Crash point: AfterOnePageFlushed
static void test_crash_after_one_page_flushed() {
    auto path = temp_test_file("crash_pt6");
    ScopedTempFile cleanup(path);
    setup_test_db(path);

    bool caught = false;
    {
        auto pf = PageFile::open(path).value();
        std::filesystem::path wal_path = path.string() + ".wal";
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        TransactionManager tm(bpm, *wal);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);
        executor.set_transaction_manager(&tm);

        EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (2, 'Bob', 200);").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (3, 'Charlie', 300);").is_ok());

        CrashSimulator::set_target(CrashPoint::AfterOnePageFlushed);
        try {
            (void)bpm.flush_all_pages();
        } catch (const CrashSimulationException&) {
            caught = true;
            bpm.drop_frames_without_flush();
        }
    }
    EXPECT_TRUE(caught);

    // Recover: partial pages reached disk, but transaction was uncommitted -> Undo restores consistent state!
    {
        auto pf = PageFile::open(path).value();
        std::filesystem::path wal_path = path.string() + ".wal";
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        RecoveryManager rec_mgr(bpm, *wal);
        EXPECT_TRUE(rec_mgr.recover().is_ok());

        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        auto sel = executor.execute("SELECT * FROM users;");
        EXPECT_TRUE(sel.is_ok());
        EXPECT_EQ(sel.value().row_count(), 1ULL);
    }
}

// 7. Crash point: AfterCommitDurableBeforeSuccess
static void test_crash_after_commit_durable_before_success() {
    auto path = temp_test_file("crash_pt7");
    ScopedTempFile cleanup(path);
    setup_test_db(path);

    bool caught = false;
    {
        auto pf = PageFile::open(path).value();
        std::filesystem::path wal_path = path.string() + ".wal";
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        TransactionManager tm(bpm, *wal);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);
        executor.set_transaction_manager(&tm);

        EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (2, 'Bob', 200);").is_ok());

        CrashSimulator::set_target(CrashPoint::AfterCommitDurableBeforeSuccess);
        try {
            (void)executor.execute("COMMIT;");
        } catch (const CrashSimulationException&) {
            caught = true;
            bpm.drop_frames_without_flush();
        }
    }
    EXPECT_TRUE(caught);

    // Recover: commit record WAS flushed durably to WAL -> Redo pass MUST restore Bob!
    {
        auto pf = PageFile::open(path).value();
        std::filesystem::path wal_path = path.string() + ".wal";
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        RecoveryManager rec_mgr(bpm, *wal);
        auto rec_res = rec_mgr.recover();
        EXPECT_TRUE(rec_res.is_ok());
        EXPECT_EQ(rec_res.value().committed_txns, 2ULL);

        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        auto sel = executor.execute("SELECT * FROM users ORDER BY id ASC;");
        EXPECT_TRUE(sel.is_ok());
        EXPECT_EQ(sel.value().row_count(), 2ULL);
        EXPECT_EQ(sel.value().rows()[1].get(1).as_text(), "Bob");
    }
}

// 8. Crash point: DuringRollback
static void test_crash_during_rollback() {
    auto path = temp_test_file("crash_pt8");
    ScopedTempFile cleanup(path);
    setup_test_db(path);

    bool caught = false;
    {
        auto pf = PageFile::open(path).value();
        std::filesystem::path wal_path = path.string() + ".wal";
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        TransactionManager tm(bpm, *wal);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);
        executor.set_transaction_manager(&tm);

        EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (2, 'Bob', 200);").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (3, 'Charlie', 300);").is_ok());

        CrashSimulator::set_target(CrashPoint::DuringRollback);
        try {
            (void)executor.execute("ROLLBACK;");
        } catch (const CrashSimulationException&) {
            caught = true;
            bpm.drop_frames_without_flush();
        }
    }
    EXPECT_TRUE(caught);

    // Recover: crashed mid-rollback -> Recovery treats it as uncommitted and completes undo!
    {
        auto pf = PageFile::open(path).value();
        std::filesystem::path wal_path = path.string() + ".wal";
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        RecoveryManager rec_mgr(bpm, *wal);
        EXPECT_TRUE(rec_mgr.recover().is_ok());

        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        auto sel = executor.execute("SELECT * FROM users;");
        EXPECT_TRUE(sel.is_ok());
        EXPECT_EQ(sel.value().row_count(), 1ULL);
    }
}

// 9. Crash point: DuringRecovery
static void test_crash_during_recovery() {
    auto path = temp_test_file("crash_pt9");
    ScopedTempFile cleanup(path);
    setup_test_db(path);

    // Setup committed transaction
    {
        auto pf = PageFile::open(path).value();
        std::filesystem::path wal_path = path.string() + ".wal";
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        TransactionManager tm(bpm, *wal);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);
        executor.set_transaction_manager(&tm);

        EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (2, 'Bob', 200);").is_ok());
        EXPECT_TRUE(executor.execute("COMMIT;").is_ok());
        bpm.drop_frames_without_flush();
    }

    // Interrupted recovery: crash during recovery
    bool caught = false;
    {
        auto pf = PageFile::open(path).value();
        std::filesystem::path wal_path = path.string() + ".wal";
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        RecoveryManager rec_mgr(bpm, *wal);

        CrashSimulator::set_target(CrashPoint::DuringRecovery);
        try {
            (void)rec_mgr.recover();
        } catch (const CrashSimulationException&) {
            caught = true;
            bpm.drop_frames_without_flush();
        }
    }
    EXPECT_TRUE(caught);

    // Re-run recovery: must be restartable and complete successfully!
    {
        auto pf = PageFile::open(path).value();
        std::filesystem::path wal_path = path.string() + ".wal";
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        RecoveryManager rec_mgr(bpm, *wal);

        EXPECT_TRUE(rec_mgr.recover().is_ok());

        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        auto sel = executor.execute("SELECT * FROM users ORDER BY id ASC;");
        EXPECT_TRUE(sel.is_ok());
        EXPECT_EQ(sel.value().row_count(), 2ULL);
        EXPECT_EQ(sel.value().rows()[1].get(1).as_text(), "Bob");
    }
}

int main() {
    std::cout << "=== KlyrDB Crash-Recovery Automated Test Suite (9 Points) ===\n\n";

    run_test("test_crash_before_page_modify", test_crash_before_page_modify);
    run_test("test_crash_after_log_before_page_flush", test_crash_after_log_before_page_flush);
    run_test("test_crash_after_memory_modify", test_crash_after_memory_modify);
    run_test("test_crash_during_wal_write", test_crash_during_wal_write);
    run_test("test_crash_during_page_flush", test_crash_during_page_flush);
    run_test("test_crash_after_one_page_flushed", test_crash_after_one_page_flushed);
    run_test("test_crash_after_commit_durable_before_success", test_crash_after_commit_durable_before_success);
    run_test("test_crash_during_rollback", test_crash_during_rollback);
    run_test("test_crash_during_recovery", test_crash_during_recovery);

    std::cout << "\nTest Results: " << g_passed_tests << " passed, "
              << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
