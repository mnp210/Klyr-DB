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
#include "klyrdb/storage/metadata_page.hpp"
#include "klyrdb/sql/executor.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_basic_checkpoint_boundary() {
    auto path = temp_test_file("test_chk_basic");
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

    EXPECT_TRUE(executor.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);").is_ok());

    // Insert 20 records under transactions
    for (int i = 1; i <= 20; ++i) {
        std::string sql = "INSERT INTO users VALUES (" + std::to_string(i) + ", 'user_" + std::to_string(i) + "');";
        EXPECT_TRUE(executor.execute(sql).is_ok());
    }

    size_t wal_size_before = wal->file_size();
    EXPECT_TRUE(wal_size_before > 0);

    // Execute Checkpoint
    CheckpointManager chk_mgr(bpm, *wal, tm);
    auto chk_res = chk_mgr.checkpoint(CheckpointMode::Safe);
    EXPECT_TRUE(chk_res.is_ok());

    const auto& stats = chk_res.value();
    EXPECT_TRUE(stats.checkpoint_lsn > 0);
    EXPECT_EQ(stats.active_txns_count, 0ULL);
    EXPECT_TRUE(stats.wal_bytes_after <= stats.wal_bytes_before);

    // Verify Page 0 metadata recorded the checkpoint LSN
    Page p0;
    EXPECT_TRUE(pf->read_page(0, p0).is_ok());
    auto meta_res = DatabaseMetadata::decode_from_page(p0);
    EXPECT_TRUE(meta_res.is_ok());
    EXPECT_EQ(meta_res.value().checkpoint_lsn, stats.checkpoint_lsn);
}

static void test_repeated_checkpoints() {
    auto path = temp_test_file("test_chk_repeated");
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

    EXPECT_TRUE(executor.execute("CREATE TABLE ledger (id INTEGER PRIMARY KEY, val INTEGER);").is_ok());

    CheckpointManager chk_mgr(bpm, *wal, tm);

    lsn_t prev_chk_lsn = 0;
    for (int batch = 1; batch <= 3; ++batch) {
        for (int i = 1; i <= 10; ++i) {
            int id = (batch - 1) * 10 + i;
            std::string sql = "INSERT INTO ledger VALUES (" + std::to_string(id) + ", " + std::to_string(id * 10) + ");";
            EXPECT_TRUE(executor.execute(sql).is_ok());
        }

        auto chk_res = chk_mgr.checkpoint(CheckpointMode::Safe);
        EXPECT_TRUE(chk_res.is_ok());
        EXPECT_TRUE(chk_res.value().checkpoint_lsn > prev_chk_lsn);
        prev_chk_lsn = chk_res.value().checkpoint_lsn;

        // Verify Page 0 reflects updated checkpoint LSN
        Page p0;
        EXPECT_TRUE(pf->read_page(0, p0).is_ok());
        auto meta_res = DatabaseMetadata::decode_from_page(p0);
        EXPECT_TRUE(meta_res.is_ok());
        EXPECT_EQ(meta_res.value().checkpoint_lsn, prev_chk_lsn);
    }

    // Verify all 30 records remain readable
    auto sel_res = executor.execute("SELECT id FROM ledger;");
    EXPECT_TRUE(sel_res.is_ok());
    EXPECT_EQ(sel_res.value().row_count(), 30ULL);
}

int main() {
    std::cout << "=== Running Checkpoint Tests ===\n";
    run_test("test_basic_checkpoint_boundary", test_basic_checkpoint_boundary);
    run_test("test_repeated_checkpoints", test_repeated_checkpoints);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
