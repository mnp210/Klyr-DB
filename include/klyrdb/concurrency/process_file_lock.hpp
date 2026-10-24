#pragma once

#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include <filesystem>
#include <chrono>
#include <memory>
#include <string>

namespace klyrdb {

/// Cross-process locking mode.
enum class ProcessLockMode {
    Shared,
    Exclusive
};

/// Cross-process operating-system-level file lock coordinator.
/// Uses Win32 LockFileEx/UnlockFileEx on Windows and fcntl on POSIX.
class ProcessFileLock {
public:
    explicit ProcessFileLock(std::filesystem::path lock_path) noexcept;
    ~ProcessFileLock();

    ProcessFileLock(ProcessFileLock&&) noexcept;
    ProcessFileLock& operator=(ProcessFileLock&&) noexcept;
    ProcessFileLock(const ProcessFileLock&) = delete;
    ProcessFileLock& operator=(const ProcessFileLock&) = delete;

    /// Attempts to acquire the lock immediately without blocking.
    /// Returns Status::database_busy() if another process holds a conflicting lock.
    [[nodiscard]] Status try_lock(ProcessLockMode mode);

    /// Attempts to acquire the lock within a specified timeout.
    /// Returns Status::lock_timeout() if timeout elapses.
    [[nodiscard]] Status lock_with_timeout(ProcessLockMode mode, std::chrono::milliseconds timeout_ms);

    /// Releases the held lock if any.
    Status unlock();

    [[nodiscard]] bool is_locked() const noexcept { return is_locked_; }
    [[nodiscard]] ProcessLockMode mode() const noexcept { return mode_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return lock_path_; }

private:
    std::filesystem::path lock_path_;
    bool is_locked_{false};
    ProcessLockMode mode_{ProcessLockMode::Shared};

#ifdef _WIN32
    void* file_handle_{nullptr}; // HANDLE
#else
    int fd_{-1};
#endif
};

} // namespace klyrdb
