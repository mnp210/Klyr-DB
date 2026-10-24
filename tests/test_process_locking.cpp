#include "test_framework.hpp"
#include "klyrdb/concurrency/process_file_lock.hpp"
#include "klyrdb/storage/page_file.hpp"
#include <chrono>

using namespace klyrdb;
using namespace klyrdb::testing;
using namespace std::chrono_literals;

static void test_process_file_lock_mutual_exclusion() {
    auto path = temp_test_file("test_proc_lock_mutex.lock");
    ScopedTempFile cleanup(path);

    ProcessFileLock lock1(path);
    EXPECT_TRUE(lock1.try_lock(ProcessLockMode::Exclusive).is_ok());
    EXPECT_TRUE(lock1.is_locked());

    // Lock 2 on same file must fail with DatabaseBusy
    ProcessFileLock lock2(path);
    auto res2 = lock2.try_lock(ProcessLockMode::Exclusive);
    EXPECT_EQ(res2.code(), StatusCode::DatabaseBusy);
    EXPECT_FALSE(lock2.is_locked());

    // Lock 2 with timeout must return LockTimeout
    auto timeout_res = lock2.lock_with_timeout(ProcessLockMode::Exclusive, 100ms);
    EXPECT_EQ(timeout_res.code(), StatusCode::LockTimeout);

    // Lock 1 unlocks
    EXPECT_TRUE(lock1.unlock().is_ok());
    EXPECT_FALSE(lock1.is_locked());

    // Lock 2 should now succeed
    EXPECT_TRUE(lock2.try_lock(ProcessLockMode::Exclusive).is_ok());
    EXPECT_TRUE(lock2.is_locked());
    EXPECT_TRUE(lock2.unlock().is_ok());
}

static void test_process_shared_vs_exclusive() {
    auto path = temp_test_file("test_proc_lock_shared_excl.lock");
    ScopedTempFile cleanup(path);

    ProcessFileLock lock1(path);
    EXPECT_TRUE(lock1.try_lock(ProcessLockMode::Shared).is_ok());

    ProcessFileLock lock2(path);
    EXPECT_TRUE(lock2.try_lock(ProcessLockMode::Shared).is_ok());

    ProcessFileLock lock3(path);
    // Exclusive request must conflict with shared locks
    auto excl_res = lock3.try_lock(ProcessLockMode::Exclusive);
    EXPECT_EQ(excl_res.code(), StatusCode::DatabaseBusy);

    EXPECT_TRUE(lock1.unlock().is_ok());
    EXPECT_TRUE(lock2.unlock().is_ok());

    // Now lock3 exclusive should succeed
    EXPECT_TRUE(lock3.try_lock(ProcessLockMode::Exclusive).is_ok());
    EXPECT_TRUE(lock3.unlock().is_ok());
}

static void test_page_file_process_lock_conflict() {
    auto path = temp_test_file("test_pf_proc_conflict.db");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");

    auto pf1_res = PageFile::create(path, true);
    EXPECT_TRUE(pf1_res.is_ok());
    auto pf1 = std::move(pf1_res.value());

    // Attempting to open pf2 while pf1 is open must fail with DatabaseBusy
    auto pf2_res = PageFile::open(path, true);
    EXPECT_EQ(pf2_res.status().code(), StatusCode::DatabaseBusy);

    // Close pf1
    EXPECT_TRUE(pf1->close().is_ok());
    pf1.reset();

    // Now opening pf2 must succeed
    pf2_res = PageFile::open(path, true);
    EXPECT_TRUE(pf2_res.is_ok());
    EXPECT_TRUE(pf2_res.value()->close().is_ok());
}

int main() {
    std::cout << "=== Running ProcessFileLock Tests ===\n";
    run_test("test_process_file_lock_mutual_exclusion", test_process_file_lock_mutual_exclusion);
    run_test("test_process_shared_vs_exclusive", test_process_shared_vs_exclusive);
    run_test("test_page_file_process_lock_conflict", test_page_file_process_lock_conflict);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
