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
#include <thread>
#include <atomic>
#include <chrono>

using namespace klyrdb;
using namespace klyrdb::testing;
using namespace std::chrono_literals;

static void test_dirty_read_prevention() {
    auto path = temp_test_file("test_dirty_read");
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

    // 1. Thread 1 begins an explicit transaction and inserts uncommitted data
    EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (1, 'Alice');").is_ok());

    // 2. Thread 2 attempts to read while transaction is uncommitted with a 50ms lock timeout
    std::atomic<bool> reader_blocked_or_timed_out{false};
    std::thread reader([&]() {
        // Create an executor with a short 50ms timeout
        SqlExecutor reader_exec(bpm, rm, *tc, *ic);
        reader_exec.set_lock_timeout(50ms);

        auto res = reader_exec.execute("SELECT * FROM users WHERE id = 1;");
        // Reader should fail due to lock contention (never observe uncommitted data)
        if (res.status().code() == StatusCode::LockTimeout || res.status().code() == StatusCode::DatabaseBusy) {
            reader_blocked_or_timed_out.store(true);
        }
    });
    reader.join();

    EXPECT_TRUE(reader_blocked_or_timed_out.load());

    // 3. Thread 1 rolls back the uncommitted transaction
    EXPECT_TRUE(executor.execute("ROLLBACK;").is_ok());

    // 4. Now reading should succeed and observe 0 rows (Alice was rolled back)
    auto res_after = executor.execute("SELECT id FROM users;");
    EXPECT_TRUE(res_after.is_ok());
    EXPECT_EQ(res_after.value().row_count(), 0ULL);
}

static void test_reader_blocks_until_writer_commits() {
    auto path = temp_test_file("test_reader_blocks_until_commit");
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

    EXPECT_TRUE(executor.execute("CREATE TABLE inventory (id INTEGER PRIMARY KEY, item TEXT, qty INTEGER);").is_ok());

    // Begin transaction on writer
    EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO inventory VALUES (10, 'Widgets', 100);").is_ok());

    std::atomic<bool> reader_started{false};
    std::atomic<bool> reader_finished{false};
    std::atomic<int64_t> reader_observed_qty{-1};

    std::thread reader([&]() {
        SqlExecutor reader_exec(bpm, rm, *tc, *ic);
        reader_exec.set_lock_timeout(3000ms);
        reader_started.store(true);

        auto res = reader_exec.execute("SELECT qty FROM inventory WHERE id = 10;");
        if (res.is_ok() && res.value().row_count() == 1) {
            reader_observed_qty.store(res.value().rows()[0].get(0).as_integer());
        }
        reader_finished.store(true);
    });

    // Wait until reader has started and is waiting on the lock
    while (!reader_started.load()) {
        std::this_thread::sleep_for(5ms);
    }
    // Give reader time to reach lock acquisition
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(reader_finished.load()); // reader must still be waiting!

    // Commit transaction
    EXPECT_TRUE(executor.execute("COMMIT;").is_ok());

    reader.join();

    EXPECT_TRUE(reader_finished.load());
    EXPECT_EQ(reader_observed_qty.load(), 100LL);
}

int main() {
    std::cout << "=== Running Transaction Isolation Tests ===\n";
    run_test("test_dirty_read_prevention", test_dirty_read_prevention);
    run_test("test_reader_blocks_until_writer_commits", test_reader_blocks_until_writer_commits);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
