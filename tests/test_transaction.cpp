#include "test_framework.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/sql/executor.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_transaction_state_transitions() {
    auto path = temp_test_file("test_txn_states");
    ScopedTempFile cleanup(path);
    std::filesystem::path wal_path = path.string() + ".wal";

    auto pf = PageFile::create(path).value();
    auto wal = WriteAheadLog::open_or_create(wal_path).value();
    BufferPoolManager bpm(*pf, 16);
    TransactionManager tm(bpm, *wal);

    // 1. Begin
    auto txn_res = tm.begin();
    EXPECT_TRUE(txn_res.is_ok());
    auto txn = txn_res.value();
    EXPECT_TRUE(txn->is_active());
    EXPECT_FALSE(txn->is_committed());
    EXPECT_FALSE(txn->is_aborted());
    EXPECT_EQ(tm.active_transaction_count(), 1ULL);

    // 2. Commit
    EXPECT_TRUE(tm.commit(txn).is_ok());
    EXPECT_TRUE(txn->is_committed());
    EXPECT_FALSE(txn->is_active());
    EXPECT_EQ(tm.active_transaction_count(), 0ULL);

    // 3. Reject double commit
    EXPECT_TRUE(tm.commit(txn).is_err());

    // 4. Reject rollback after commit
    EXPECT_TRUE(tm.rollback(txn).is_err());

    // 5. Begin and Rollback
    auto txn2_res = tm.begin();
    EXPECT_TRUE(txn2_res.is_ok());
    auto txn2 = txn2_res.value();
    EXPECT_TRUE(tm.rollback(txn2).is_ok());
    EXPECT_TRUE(txn2->is_aborted());
    EXPECT_FALSE(txn2->is_active());

    // 6. Reject commit after rollback
    EXPECT_TRUE(tm.commit(txn2).is_err());
}

static void test_transaction_multi_page_rollback() {
    auto path = temp_test_file("test_txn_multi_rollback");
    ScopedTempFile cleanup(path);
    std::filesystem::path wal_path = path.string() + ".wal";

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

    // Create table outside transaction
    EXPECT_TRUE(executor.execute("CREATE TABLE accounts (id INTEGER, balance INTEGER, owner TEXT);").is_ok());

    // Populate baseline row
    EXPECT_TRUE(executor.execute("INSERT INTO accounts VALUES (1, 1000, 'Alice');").is_ok());

    // Begin explicit transaction
    EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
    EXPECT_TRUE(tm.current_transaction() != nullptr);

    // Perform multiple inserts and updates affecting multiple pages
    EXPECT_TRUE(executor.execute("INSERT INTO accounts VALUES (2, 500, 'Bob');").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO accounts VALUES (3, 750, 'Charlie');").is_ok());
    EXPECT_TRUE(executor.execute("UPDATE accounts SET balance = 1200 WHERE id = 1;").is_ok());

    // Verify changes are visible inside transaction
    {
        auto sel = executor.execute("SELECT * FROM accounts;");
        EXPECT_TRUE(sel.is_ok());
        EXPECT_EQ(sel.value().row_count(), 3ULL);
    }

    // ROLLBACK!
    EXPECT_TRUE(executor.execute("ROLLBACK;").is_ok());
    EXPECT_TRUE(tm.current_transaction() == nullptr);

    // Verify state after rollback: exactly 1 baseline row with original balance
    {
        auto sel = executor.execute("SELECT * FROM accounts;");
        EXPECT_TRUE(sel.is_ok());
        EXPECT_EQ(sel.value().row_count(), 1ULL);
    }
    {
        auto sel = executor.execute("SELECT balance FROM accounts WHERE id = 1;");
        EXPECT_TRUE(sel.is_ok());
        EXPECT_EQ(sel.value().rows()[0].get(0).as_integer(), 1000LL);
    }
}

static void test_transaction_multiple_updates_same_page_rollback() {
    auto path = temp_test_file("test_txn_same_page");
    ScopedTempFile cleanup(path);
    std::filesystem::path wal_path = path.string() + ".wal";

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

    EXPECT_TRUE(executor.execute("CREATE TABLE counter (id INTEGER, val INTEGER);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO counter VALUES (1, 10);").is_ok());

    // Explicit transaction modifying the same page 3 times
    EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
    EXPECT_TRUE(executor.execute("UPDATE counter SET val = 20 WHERE id = 1;").is_ok());
    EXPECT_TRUE(executor.execute("UPDATE counter SET val = 30 WHERE id = 1;").is_ok());
    EXPECT_TRUE(executor.execute("UPDATE counter SET val = 40 WHERE id = 1;").is_ok());

    // Rollback
    EXPECT_TRUE(executor.execute("ROLLBACK;").is_ok());

    // Verify value restored back to 10
    auto sel = executor.execute("SELECT val FROM counter WHERE id = 1;");
    EXPECT_TRUE(sel.is_ok());
    EXPECT_EQ(sel.value().rows()[0].get(0).as_integer(), 10LL);
}

static void test_transaction_wal_flush_ordering() {
    auto path = temp_test_file("test_txn_flush_order");
    ScopedTempFile cleanup(path);
    std::filesystem::path wal_path = path.string() + ".wal";

    auto pf = PageFile::create(path).value();
    auto wal = WriteAheadLog::open_or_create(wal_path).value();
    BufferPoolManager bpm(*pf, 16);
    TransactionManager tm(bpm, *wal);

    auto txn_res = tm.begin();
    EXPECT_TRUE(txn_res.is_ok());
    auto txn = txn_res.value();
    tm.set_current_transaction(txn);

    page_id_t pid = 0;
    auto guard_res = bpm.new_page_guard(pid);
    EXPECT_TRUE(guard_res.is_ok());
    auto& guard = guard_res.value();
    guard->set_page_type(PageType::Data);
    guard.mark_dirty();
    guard.drop(); // Unpins and logs PageUpdate to WAL

    // Page LSN should now be > 0
    EXPECT_TRUE(wal->last_lsn() > 0);

    // Flushing page must ensure WAL is flushed up to page LSN before page reaches disk
    EXPECT_TRUE(bpm.flush_page(pid).is_ok());
    EXPECT_TRUE(wal->flushed_lsn() >= wal->last_lsn());

    (void)tm.commit(txn);
}

static void test_transaction_ddl_rejection_in_explicit_block() {
    auto path = temp_test_file("test_txn_ddl");
    ScopedTempFile cleanup(path);
    std::filesystem::path wal_path = path.string() + ".wal";

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

    EXPECT_TRUE(executor.execute("BEGIN;").is_ok());

    // DDL statements must be explicitly rejected inside an explicit transaction block
    auto res_ct = executor.execute("CREATE TABLE test (id INTEGER);");
    EXPECT_TRUE(res_ct.is_err());
    EXPECT_EQ(res_ct.status().code(), StatusCode::InvalidTransactionState);

    auto res_ci = executor.execute("CREATE INDEX idx ON test (id);");
    EXPECT_TRUE(res_ci.is_err());
    EXPECT_EQ(res_ci.status().code(), StatusCode::InvalidTransactionState);

    EXPECT_TRUE(executor.execute("ROLLBACK;").is_ok());
}

int main() {
    std::cout << "=== KlyrDB Transaction Management Test Suite ===\n\n";

    run_test("test_transaction_state_transitions", test_transaction_state_transitions);
    run_test("test_transaction_multi_page_rollback", test_transaction_multi_page_rollback);
    run_test("test_transaction_multiple_updates_same_page_rollback", test_transaction_multiple_updates_same_page_rollback);
    run_test("test_transaction_wal_flush_ordering", test_transaction_wal_flush_ordering);
    run_test("test_transaction_ddl_rejection_in_explicit_block", test_transaction_ddl_rejection_in_explicit_block);

    std::cout << "\nTest Results: " << g_passed_tests << " passed, "
              << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
