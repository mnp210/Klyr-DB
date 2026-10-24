#include "test_framework.hpp"
#include "klyrdb/concurrency/database_lock.hpp"
#include "klyrdb/concurrency/lock_manager.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/recovery/recovery_manager.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

using namespace klyrdb;
using namespace klyrdb::testing;
using namespace std::chrono_literals;

static void test_basic_shared_lock() {
    auto path = temp_test_file("test_db_lock_basic_shared");
    ScopedTempFile cleanup(path);

    DatabaseLock lock(path, false);
    EXPECT_EQ(lock.shared_reader_count(), 0ULL);
    EXPECT_FALSE(lock.is_exclusively_locked());

    EXPECT_TRUE(lock.acquire_shared(1000ms).is_ok());
    EXPECT_EQ(lock.shared_reader_count(), 1ULL);
    EXPECT_FALSE(lock.is_exclusively_locked());

    EXPECT_TRUE(lock.release_shared().is_ok());
    EXPECT_EQ(lock.shared_reader_count(), 0ULL);
}

static void test_concurrent_shared_locks() {
    auto path = temp_test_file("test_db_lock_concurrent_shared");
    ScopedTempFile cleanup(path);

    DatabaseLock lock(path, false);
    constexpr size_t num_threads = 4;
    std::atomic<size_t> ready_count{0};
    std::atomic<bool> hold_locks{true};
    std::vector<std::thread> threads;

    for (size_t i = 0; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            auto res = lock.acquire_shared(2000ms);
            if (res.is_ok()) {
                ready_count.fetch_add(1);
                while (hold_locks.load()) {
                    std::this_thread::sleep_for(5ms);
                }
                lock.release_shared();
            }
        });
    }

    // Wait until all threads acquired shared locks
    auto start = std::chrono::steady_clock::now();
    while (ready_count.load() < num_threads &&
           std::chrono::steady_clock::now() - start < 3000ms) {
        std::this_thread::sleep_for(10ms);
    }

    EXPECT_EQ(ready_count.load(), num_threads);
    EXPECT_EQ(lock.shared_reader_count(), num_threads);
    EXPECT_FALSE(lock.is_exclusively_locked());

    hold_locks.store(false);
    for (auto& t : threads) {
        t.join();
    }
    EXPECT_EQ(lock.shared_reader_count(), 0ULL);
}

static void test_exclusive_lock_mutual_exclusion() {
    auto path = temp_test_file("test_db_lock_exclusive_mutex");
    ScopedTempFile cleanup(path);

    DatabaseLock lock(path, false);

    EXPECT_TRUE(lock.acquire_exclusive(1000ms).is_ok());
    EXPECT_TRUE(lock.is_exclusively_locked());
    EXPECT_TRUE(lock.is_held_by_current_thread_exclusive());

    // Another thread attempting shared lock must timeout
    std::atomic<bool> thread2_timed_out{false};
    std::thread reader([&]() {
        auto res = lock.acquire_shared(100ms);
        if (res.code() == StatusCode::LockTimeout) {
            thread2_timed_out.store(true);
        } else if (res.is_ok()) {
            lock.release_shared();
        }
    });
    reader.join();
    EXPECT_TRUE(thread2_timed_out.load());

    // Another thread attempting exclusive lock must timeout
    std::atomic<bool> thread3_timed_out{false};
    std::thread writer([&]() {
        auto res = lock.acquire_exclusive(100ms);
        if (res.code() == StatusCode::LockTimeout) {
            thread3_timed_out.store(true);
        } else if (res.is_ok()) {
            lock.release_exclusive();
        }
    });
    writer.join();
    EXPECT_TRUE(thread3_timed_out.load());

    // Main thread releases exclusive lock
    EXPECT_TRUE(lock.release_exclusive().is_ok());
    EXPECT_FALSE(lock.is_exclusively_locked());
}

static void test_shared_blocks_exclusive() {
    auto path = temp_test_file("test_db_lock_shared_blocks_excl");
    ScopedTempFile cleanup(path);

    DatabaseLock lock(path, false);
    EXPECT_TRUE(lock.acquire_shared(1000ms).is_ok());

    std::atomic<bool> writer_timed_out{false};
    std::thread writer([&]() {
        auto res = lock.acquire_exclusive(100ms);
        if (res.code() == StatusCode::LockTimeout) {
            writer_timed_out.store(true);
        } else if (res.is_ok()) {
            lock.release_exclusive();
        }
    });
    writer.join();
    EXPECT_TRUE(writer_timed_out.load());

    EXPECT_TRUE(lock.release_shared().is_ok());

    // Now writer can acquire
    EXPECT_TRUE(lock.acquire_exclusive(1000ms).is_ok());
    EXPECT_TRUE(lock.release_exclusive().is_ok());
}

static void test_writer_reentrancy() {
    auto path = temp_test_file("test_db_lock_reentrancy");
    ScopedTempFile cleanup(path);

    DatabaseLock lock(path, false);

    EXPECT_TRUE(lock.acquire_exclusive(1000ms).is_ok());
    EXPECT_TRUE(lock.is_held_by_current_thread_exclusive());

    // Same thread acquires exclusive again
    EXPECT_TRUE(lock.acquire_exclusive(1000ms).is_ok());
    EXPECT_TRUE(lock.is_held_by_current_thread_exclusive());

    // Same thread acquires shared (permitted when holding exclusive)
    EXPECT_TRUE(lock.acquire_shared(1000ms).is_ok());
    EXPECT_TRUE(lock.release_shared().is_ok());

    // Release both exclusive recursions
    EXPECT_TRUE(lock.release_exclusive().is_ok());
    EXPECT_TRUE(lock.is_held_by_current_thread_exclusive()); // still held once
    EXPECT_TRUE(lock.release_exclusive().is_ok());
    EXPECT_FALSE(lock.is_exclusively_locked());
}

static void test_raii_guards() {
    auto path = temp_test_file("test_db_lock_raii");
    ScopedTempFile cleanup(path);

    DatabaseLock lock(path, false);

    {
        auto shared_guard_res = SharedLockGuard::acquire(lock, 1000ms);
        EXPECT_TRUE(shared_guard_res.is_ok());
        EXPECT_EQ(lock.shared_reader_count(), 1ULL);
    }
    EXPECT_EQ(lock.shared_reader_count(), 0ULL);

    {
        auto excl_guard_res = ExclusiveLockGuard::acquire(lock, 1000ms);
        EXPECT_TRUE(excl_guard_res.is_ok());
        EXPECT_TRUE(lock.is_exclusively_locked());

        // Early unlock
        excl_guard_res.value().unlock();
        EXPECT_FALSE(lock.is_exclusively_locked());
    }
    EXPECT_FALSE(lock.is_exclusively_locked());
}

static void test_recovery_lock_coordination() {
    auto path = temp_test_file("test_recovery_lock_coord");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);

    auto pf = PageFile::create(path, false).value();
    auto wal = WriteAheadLog::open_or_create(wal_path).value();
    BufferPoolManager bpm(*pf, 16);

    auto dblock = LockManager::instance().get_lock(path, false);
    dblock->set_default_timeout(50ms);

    // Acquire shared lock on database
    EXPECT_TRUE(dblock->acquire_shared().is_ok());

    // In a separate thread, attempt recovery: it should fail with LockTimeout because shared lock is held
    std::atomic<bool> recovery_failed_with_timeout{false};
    std::thread recovery_worker([&]() {
        RecoveryManager rm(bpm, *wal);
        auto res = rm.recover();
        if (res.status().code() == StatusCode::LockTimeout) {
            recovery_failed_with_timeout.store(true);
        }
    });
    recovery_worker.join();

    EXPECT_TRUE(recovery_failed_with_timeout.load());

    // Release shared lock
    EXPECT_TRUE(dblock->release_shared().is_ok());

    // Now recovery should succeed
    RecoveryManager rm(bpm, *wal);
    auto res2 = rm.recover();
    EXPECT_TRUE(res2.is_ok());
}

static void test_lock_manager_singleton() {
    auto path1 = temp_test_file("test_lock_mgr_1");
    auto path2 = temp_test_file("test_lock_mgr_2");
    ScopedTempFile cleanup1(path1);
    ScopedTempFile cleanup2(path2);

    auto& lm = LockManager::instance();
    auto lock1_a = lm.get_lock(path1, false);
    auto lock1_b = lm.get_lock(path1, false);
    auto lock2 = lm.get_lock(path2, false);

    EXPECT_EQ(lock1_a.get(), lock1_b.get());
    EXPECT_NE(lock1_a.get(), lock2.get());

    lock1_a.reset();
    lock1_b.reset();
    lock2.reset();
    lm.clean_unused_locks();
}

int main() {
    std::cout << "=== Running DatabaseLock Tests ===\n";
    run_test("test_basic_shared_lock", test_basic_shared_lock);
    run_test("test_concurrent_shared_locks", test_concurrent_shared_locks);
    run_test("test_exclusive_lock_mutual_exclusion", test_exclusive_lock_mutual_exclusion);
    run_test("test_shared_blocks_exclusive", test_shared_blocks_exclusive);
    run_test("test_writer_reentrancy", test_writer_reentrancy);
    run_test("test_raii_guards", test_raii_guards);
    run_test("test_recovery_lock_coordination", test_recovery_lock_coordination);
    run_test("test_lock_manager_singleton", test_lock_manager_singleton);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
