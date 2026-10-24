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
#include "klyrdb/sql/executor.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_wal_retention_with_active_transaction() {
    auto path = temp_test_file("test_retention_active");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);

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

    EXPECT_TRUE(executor.execute("CREATE TABLE accounts (id INTEGER PRIMARY KEY, balance INTEGER);").is_ok());

    // Insert baseline committed rows
    EXPECT_TRUE(executor.execute("INSERT INTO accounts VALUES (1, 100);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO accounts VALUES (2, 200);").is_ok());

    // 1. Begin long-running transaction T1
    EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO accounts VALUES (3, 300);").is_ok());
    EXPECT_TRUE(executor.execute("UPDATE accounts SET balance = 150 WHERE id = 1;").is_ok());
    EXPECT_EQ(tm.active_transaction_count(), 1ULL);

    // 2. Checkpoint runs while T1 is still active
    CheckpointManager chk_mgr(bpm, *wal, tm);
    auto chk_res = chk_mgr.checkpoint(CheckpointMode::Safe);
    EXPECT_TRUE(chk_res.is_ok());

    const auto& s = chk_res.value();
    EXPECT_EQ(s.active_txns_count, 1ULL);
    EXPECT_TRUE(s.min_required_lsn > 0);

    // 3. Verify WAL still contains T1's records
    auto recs_res = wal->read_all_records();
    EXPECT_TRUE(recs_res.is_ok());
    bool found_t1_record = false;
    for (const auto& r : recs_res.value()) {
        if (r.txn_id == 3 || (r.type == LogRecordType::PageUpdate && r.lsn >= s.min_required_lsn)) {
            found_t1_record = true;
            break;
        }
    }
    EXPECT_TRUE(found_t1_record);

    // 4. Commit T1
    EXPECT_TRUE(executor.execute("COMMIT;").is_ok());
    EXPECT_EQ(tm.active_transaction_count(), 0ULL);

    // Verify final state
    auto r1 = executor.execute("SELECT balance FROM accounts WHERE id = 1;");
    EXPECT_TRUE(r1.is_ok());
    EXPECT_EQ(r1.value().rows()[0].get(0).as_integer(), 150LL);

    auto r3 = executor.execute("SELECT balance FROM accounts WHERE id = 3;");
    EXPECT_TRUE(r3.is_ok());
    EXPECT_EQ(r3.value().rows()[0].get(0).as_integer(), 300LL);
}

static void test_wal_retention_rollback_after_checkpoint() {
    auto path = temp_test_file("test_retention_rollback");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);

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

    EXPECT_TRUE(executor.execute("CREATE TABLE vault (id INTEGER PRIMARY KEY, secret TEXT);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO vault VALUES (1, 'initial_state');").is_ok());

    // Begin active transaction
    EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
    EXPECT_TRUE(executor.execute("UPDATE vault SET secret = 'tampered' WHERE id = 1;").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO vault VALUES (2, 'uncommitted_row');").is_ok());

    // Run checkpoint while active
    CheckpointManager chk_mgr(bpm, *wal, tm);
    auto chk_res = chk_mgr.checkpoint(CheckpointMode::Safe);
    EXPECT_TRUE(chk_res.is_ok());

    // Rollback active transaction: undo records MUST be available in memory and WAL!
    EXPECT_TRUE(executor.execute("ROLLBACK;").is_ok());

    // Verify rollback restored initial state
    auto r1 = executor.execute("SELECT secret FROM vault WHERE id = 1;");
    EXPECT_TRUE(r1.is_ok());
    EXPECT_EQ(r1.value().rows()[0].get(0).as_text(), "initial_state");

    auto r2 = executor.execute("SELECT id FROM vault WHERE id = 2;");
    EXPECT_TRUE(r2.is_ok());
    EXPECT_EQ(r2.value().row_count(), 0ULL);
}

int main() {
    std::cout << "=== Running WAL Retention Tests ===\n";
    run_test("test_wal_retention_with_active_transaction", test_wal_retention_with_active_transaction);
    run_test("test_wal_retention_rollback_after_checkpoint", test_wal_retention_rollback_after_checkpoint);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
