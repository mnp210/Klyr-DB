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
#include "klyrdb/sql/executor.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_recovery_committed_transaction_redo() {
    auto path = temp_test_file("test_rec_redo");
    ScopedTempFile cleanup(path);
    std::filesystem::path wal_path = path.string() + ".wal";

    // 1. Initial execution: insert rows, commit, but DO NOT flush buffer pool to disk
    {
        auto pf = PageFile::create(path).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        TransactionManager tm(bpm, *wal);

        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();

        SqlExecutor executor(bpm, rm, *tc, *ic);
        executor.set_transaction_manager(&tm);

        EXPECT_TRUE(executor.execute("CREATE TABLE products (id INTEGER, name TEXT, price INTEGER);").is_ok());
        (void)bpm.flush_all_pages(); // Ensure table catalog is flushed before transaction

        // Committed transaction
        EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO products VALUES (1, 'Widget', 25);").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO products VALUES (2, 'Gadget', 45);").is_ok());
        EXPECT_TRUE(executor.execute("COMMIT;").is_ok());

        // Simulate crash: drop in-memory frames without flushing dirty pages!
        bpm.drop_frames_without_flush();
    }

    // 2. Reopen and recover
    {
        auto pf = PageFile::open(path).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);

        RecoveryManager rec_mgr(bpm, *wal);
        EXPECT_TRUE(rec_mgr.needs_recovery());

        auto stats_res = rec_mgr.recover();
        EXPECT_TRUE(stats_res.is_ok());
        EXPECT_TRUE(stats_res.value().pages_redone > 0);
        EXPECT_EQ(stats_res.value().committed_txns, 1ULL);
        EXPECT_EQ(stats_res.value().uncommitted_txns, 0ULL);
    }

    // 3. Reopen and verify recovered data
    {
        auto pf = PageFile::open(path).value();
        BufferPoolManager bpm(*pf, 32);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        auto sel = executor.execute("SELECT * FROM products ORDER BY id ASC;");
        EXPECT_TRUE(sel.is_ok());
        EXPECT_EQ(sel.value().row_count(), 2ULL);
        EXPECT_EQ(sel.value().rows()[0].get(1).as_text(), "Widget");
        EXPECT_EQ(sel.value().rows()[1].get(1).as_text(), "Gadget");
    }
}

static void test_recovery_uncommitted_transaction_undo() {
    auto path = temp_test_file("test_rec_undo");
    ScopedTempFile cleanup(path);
    std::filesystem::path wal_path = path.string() + ".wal";

    // 1. Setup baseline data
    {
        auto pf = PageFile::create(path).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        TransactionManager tm(bpm, *wal);

        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);
        executor.set_transaction_manager(&tm);

        EXPECT_TRUE(executor.execute("CREATE TABLE users (id INTEGER, name TEXT);").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (1, 'Alice');").is_ok());
        (void)bpm.flush_all_pages();

        // 2. Incomplete transaction: update and insert, flushed to disk, but NO COMMIT
        EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
        EXPECT_TRUE(executor.execute("UPDATE users SET name = 'Alice_Mod' WHERE id = 1;").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (2, 'Bob_Temp');").is_ok());

        // Flush dirty pages to disk while transaction is still uncommitted!
        (void)bpm.flush_all_pages();
        // Drop frames without commit or abort (crash!)
        bpm.drop_frames_without_flush();
    }

    // 3. Reopen and recover: Undo must restore Alice and discard Bob_Temp
    {
        auto pf = PageFile::open(path).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);

        RecoveryManager rec_mgr(bpm, *wal);
        EXPECT_TRUE(rec_mgr.needs_recovery());

        auto stats_res = rec_mgr.recover();
        EXPECT_TRUE(stats_res.is_ok());
        EXPECT_TRUE(stats_res.value().pages_undone > 0);
        EXPECT_EQ(stats_res.value().uncommitted_txns, 1ULL);
    }

    // 4. Verify uncommitted changes were safely undone
    {
        auto pf = PageFile::open(path).value();
        BufferPoolManager bpm(*pf, 32);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        auto sel = executor.execute("SELECT * FROM users;");
        EXPECT_TRUE(sel.is_ok());
        EXPECT_EQ(sel.value().row_count(), 1ULL);
        EXPECT_EQ(sel.value().rows()[0].get(1).as_text(), "Alice");
    }
}

static void test_recovery_restartability() {
    auto path = temp_test_file("test_rec_restart");
    ScopedTempFile cleanup(path);
    std::filesystem::path wal_path = path.string() + ".wal";

    {
        auto pf = PageFile::create(path).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        TransactionManager tm(bpm, *wal);

        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);
        executor.set_transaction_manager(&tm);

        EXPECT_TRUE(executor.execute("CREATE TABLE t (id INTEGER);").is_ok());
        (void)bpm.flush_all_pages();
        EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO t VALUES (42);").is_ok());
        EXPECT_TRUE(executor.execute("COMMIT;").is_ok());
        bpm.drop_frames_without_flush();
    }

    // Run recovery 1st time
    {
        auto pf = PageFile::open(path).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        RecoveryManager rec_mgr(bpm, *wal);
        auto r1 = rec_mgr.recover();
        EXPECT_TRUE(r1.is_ok());
    }

    // Run recovery 2nd time on recovered database
    {
        auto pf = PageFile::open(path).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        RecoveryManager rec_mgr(bpm, *wal);
        auto r2 = rec_mgr.recover();
        EXPECT_TRUE(r2.is_ok());
    }

    // Verify row 42 exists
    {
        auto pf = PageFile::open(path).value();
        BufferPoolManager bpm(*pf, 32);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        auto sel = executor.execute("SELECT * FROM t;");
        EXPECT_TRUE(sel.is_ok());
        EXPECT_EQ(sel.value().row_count(), 1ULL);
        EXPECT_EQ(sel.value().rows()[0].get(0).as_integer(), 42LL);
    }
}

int main() {
    std::cout << "=== KlyrDB Crash Recovery Engine Test Suite ===\n\n";

    run_test("test_recovery_committed_transaction_redo", test_recovery_committed_transaction_redo);
    run_test("test_recovery_uncommitted_transaction_undo", test_recovery_uncommitted_transaction_undo);
    run_test("test_recovery_restartability", test_recovery_restartability);

    std::cout << "\nTest Results: " << g_passed_tests << " passed, "
              << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
