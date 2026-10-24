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
#include "klyrdb/recovery/checkpoint_manager.hpp"
#include "klyrdb/storage/integrity_checker.hpp"
#include "klyrdb/sql/executor.hpp"
#include <thread>
#include <atomic>
#include <vector>

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_concurrent_readers_and_writer_stress() {
    auto path = temp_test_file("test_rel_stress_readers");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);

    auto pf = PageFile::create(path, false).value();
    auto wal = WriteAheadLog::open_or_create(wal_path).value();
    BufferPoolManager bpm(*pf, 64);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();
    TransactionManager tm(bpm, *wal);

    SqlExecutor executor(bpm, rm, *tc, *ic);
    executor.set_transaction_manager(&tm);

    EXPECT_TRUE(executor.execute("CREATE TABLE inventory (id INTEGER PRIMARY KEY, qty INTEGER);").is_ok());
    for (int i = 1; i <= 50; ++i) {
        EXPECT_TRUE(executor.execute("INSERT INTO inventory VALUES (" + std::to_string(i) + ", 100);").is_ok());
    }

    std::atomic<bool> stop_flag{false};
    std::atomic<uint64_t> read_ops{0};

    // Spawn 3 concurrent readers
    auto reader_fn = [&]() {
        while (!stop_flag.load(std::memory_order_relaxed)) {
            auto res = executor.execute("SELECT * FROM inventory WHERE id = 25;");
            if (res.is_ok()) {
                read_ops.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };

    std::vector<std::thread> readers;
    for (int i = 0; i < 3; ++i) {
        readers.emplace_back(reader_fn);
    }

    // Writer performs 50 transactions with commits and rollbacks
    for (int i = 0; i < 50; ++i) {
        EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
        EXPECT_TRUE(executor.execute("UPDATE inventory SET qty = 150 WHERE id = 10;").is_ok());
        if (i % 2 == 0) {
            EXPECT_TRUE(executor.execute("COMMIT;").is_ok());
        } else {
            EXPECT_TRUE(executor.execute("ROLLBACK;").is_ok());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    stop_flag.store(true);
    for (auto& t : readers) {
        t.join();
    }

    EXPECT_TRUE(read_ops.load() > 10ULL);

    EXPECT_TRUE(bpm.flush_all_pages().is_ok());
    EXPECT_TRUE(pf->sync().is_ok());

    // Verify final integrity
    IntegrityChecker checker(*pf, wal.get());
    auto rep = checker.check_all().value();
    EXPECT_TRUE(rep.is_healthy);
}

static void test_crash_recovery_stress() {
    auto path = temp_test_file("test_rel_stress_crash");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);

    for (int round = 0; round < 3; ++round) {
        // Open and run workload with uncommitted transactions
        {
            auto pf = (round == 0) ? PageFile::create(path, false).value() : PageFile::open(path, false).value();
            auto wal = WriteAheadLog::open_or_create(wal_path).value();
            BufferPoolManager bpm(*pf, 32);

            // Run recovery if needed
            RecoveryManager rec(bpm, *wal);
            if (rec.needs_recovery()) {
                EXPECT_TRUE(rec.recover().is_ok());
            }

            auto fsm = FreeSpaceMap::open_or_create(bpm).value();
            RecordManager rm(bpm, fsm.get());
            auto ic = IndexCatalog::open_or_create(bpm).value();
            auto tc = TableCatalog::open_or_create(bpm).value();
            TransactionManager tm(bpm, *wal);

            SqlExecutor executor(bpm, rm, *tc, *ic);
            executor.set_transaction_manager(&tm);

            if (round == 0) {
                EXPECT_TRUE(executor.execute("CREATE TABLE stress (id INTEGER PRIMARY KEY, val INTEGER);").is_ok());
            }

            // Committed writes
            EXPECT_TRUE(executor.execute("INSERT INTO stress VALUES (" + std::to_string(round * 10 + 1) + ", 100);").is_ok());

            // Uncommitted transaction left active before simulated crash
            EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
            EXPECT_TRUE(executor.execute("INSERT INTO stress VALUES (" + std::to_string(round * 10 + 2) + ", 200);").is_ok());

            // Simulate sudden crash: drop in-memory frames without flushing
            bpm.drop_frames_without_flush();
        }

        // Post-crash restart and recovery
        {
            auto pf = PageFile::open(path, false).value();
            auto wal = WriteAheadLog::open_or_create(wal_path).value();
            BufferPoolManager bpm(*pf, 32);

            RecoveryManager rec(bpm, *wal);
            EXPECT_TRUE(rec.needs_recovery());
            auto rec_res = rec.recover();
            EXPECT_TRUE(rec_res.is_ok());

            // Verify integrity
            IntegrityChecker checker(*pf, wal.get());
            auto rep = checker.check_all().value();
            EXPECT_TRUE(rep.is_healthy);
            EXPECT_EQ(rep.error_count(), 0ULL);
        }
    }
}

int main() {
    std::cout << "=== Running Reliability, Concurrency & Crash Stress Tests ===\n";

    run_test("test_concurrent_readers_and_writer_stress", test_concurrent_readers_and_writer_stress);
    run_test("test_crash_recovery_stress", test_crash_recovery_stress);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
