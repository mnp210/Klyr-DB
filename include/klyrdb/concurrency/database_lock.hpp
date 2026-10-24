#pragma once

#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/concurrency/process_file_lock.hpp"
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <filesystem>
#include <memory>

namespace klyrdb {

enum class DBLockMode {
    Shared,
    Exclusive
};

/// High-level database lock coordinator managing both in-process
/// Single-Writer / Multiple-Reader (SWMR) synchronization and
/// cross-process OS file locks.
class DatabaseLock {
public:
    explicit DatabaseLock(std::filesystem::path db_path, bool enable_process_lock = true);
    ~DatabaseLock();

    DatabaseLock(const DatabaseLock&) = delete;
    DatabaseLock& operator=(const DatabaseLock&) = delete;
    DatabaseLock(DatabaseLock&&) = delete;
    DatabaseLock& operator=(DatabaseLock&&) = delete;

    /// In-process shared lock acquisition (for readers).
    Status acquire_shared(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    /// In-process shared lock release.
    Status release_shared();

    /// In-process exclusive lock acquisition (for writers, recovery, DDL).
    /// Supports reentrancy for the same thread (e.g. within an explicit transaction).
    Status acquire_exclusive(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    /// In-process exclusive lock release.
    Status release_exclusive();

    /// Cross-process OS file lock acquisition.
    Status acquire_process_lock(ProcessLockMode mode, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    /// Cross-process OS file lock release.
    Status release_process_lock();

    [[nodiscard]] bool is_held_by_current_thread_exclusive() const noexcept;
    [[nodiscard]] bool is_exclusively_locked() const noexcept;
    [[nodiscard]] size_t shared_reader_count() const noexcept;

    void set_default_timeout(std::chrono::milliseconds timeout) noexcept { default_timeout_ = timeout; }
    [[nodiscard]] std::chrono::milliseconds default_timeout() const noexcept { return default_timeout_; }

    [[nodiscard]] const std::filesystem::path& db_path() const noexcept { return db_path_; }

private:
    std::filesystem::path db_path_;
    bool enable_process_lock_{true};
    ProcessFileLock process_lock_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    size_t shared_readers_{0};
    size_t waiting_writers_{0};
    bool exclusive_active_{false};
    std::thread::id exclusive_owner_{};
    size_t exclusive_recursion_{0};

    std::chrono::milliseconds default_timeout_{5000};
};

/// RAII Guard for shared (read-only) database locking.
class SharedLockGuard {
public:
    [[nodiscard]] static Result<SharedLockGuard> acquire(
        DatabaseLock& lock,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    ~SharedLockGuard();

    SharedLockGuard(SharedLockGuard&& other) noexcept;
    SharedLockGuard& operator=(SharedLockGuard&& other) noexcept;
    SharedLockGuard(const SharedLockGuard&) = delete;
    SharedLockGuard& operator=(const SharedLockGuard&) = delete;

private:
    explicit SharedLockGuard(DatabaseLock& lock) noexcept : lock_(&lock), active_(true) {}

    DatabaseLock* lock_{nullptr};
    bool active_{false};
};

/// RAII Guard for exclusive (read-write) database locking.
class ExclusiveLockGuard {
public:
    [[nodiscard]] static Result<ExclusiveLockGuard> acquire(
        DatabaseLock& lock,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    ~ExclusiveLockGuard();

    ExclusiveLockGuard(ExclusiveLockGuard&& other) noexcept;
    ExclusiveLockGuard& operator=(ExclusiveLockGuard&& other) noexcept;
    ExclusiveLockGuard(const ExclusiveLockGuard&) = delete;
    ExclusiveLockGuard& operator=(const ExclusiveLockGuard&) = delete;

    /// Releases the lock early before destruction.
    void unlock();

private:
    explicit ExclusiveLockGuard(DatabaseLock& lock) noexcept : lock_(&lock), active_(true) {}

    DatabaseLock* lock_{nullptr};
    bool active_{false};
};

} // namespace klyrdb
