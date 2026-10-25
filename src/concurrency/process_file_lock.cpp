#include "klyrdb/concurrency/process_file_lock.hpp"
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#endif

namespace klyrdb {

ProcessFileLock::ProcessFileLock(std::filesystem::path lock_path) noexcept
    : lock_path_(std::move(lock_path)) {}

ProcessFileLock::~ProcessFileLock() {
    (void)unlock();
}

ProcessFileLock::ProcessFileLock(ProcessFileLock&& other) noexcept
    : lock_path_(std::move(other.lock_path_)),
      is_locked_(other.is_locked_),
      mode_(other.mode_) {
#ifdef _WIN32
    file_handle_ = other.file_handle_;
    other.file_handle_ = nullptr;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
    other.is_locked_ = false;
}

ProcessFileLock& ProcessFileLock::operator=(ProcessFileLock&& other) noexcept {
    if (this != &other) {
        (void)unlock();
        lock_path_ = std::move(other.lock_path_);
        is_locked_ = other.is_locked_;
        mode_ = other.mode_;
#ifdef _WIN32
        file_handle_ = other.file_handle_;
        other.file_handle_ = nullptr;
#else
        fd_ = other.fd_;
        other.fd_ = -1;
#endif
        other.is_locked_ = false;
    }
    return *this;
}

Status ProcessFileLock::try_lock(ProcessLockMode mode) {
    if (is_locked_) {
        if (mode_ == mode) return Status::ok();
        return Status::invalid_lock_state("Process lock already held in a different mode");
    }

#ifdef _WIN32
    if (!file_handle_) {
        // Ensure parent directory exists
        if (lock_path_.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(lock_path_.parent_path(), ec);
        }

        file_handle_ = CreateFileW(
            lock_path_.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (file_handle_ == INVALID_HANDLE_VALUE) {
            file_handle_ = nullptr;
            DWORD err = GetLastError();
            if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION) {
                return Status::database_busy("Database lock file is open by another process: " + lock_path_.string());
            }
            return Status::io_error("Failed to open lock file: " + lock_path_.string() + " (error " + std::to_string(err) + ")");
        }
    }

    OVERLAPPED ov{};
    DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;
    if (mode == ProcessLockMode::Exclusive) {
        flags |= LOCKFILE_EXCLUSIVE_LOCK;
    }

    BOOL ok = LockFileEx(static_cast<HANDLE>(file_handle_), flags, 0, 1, 0, &ov);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_LOCK_VIOLATION || err == ERROR_SHARING_VIOLATION) {
            return Status::database_busy("Database lock conflict: held by another process: " + lock_path_.string());
        }
        return Status::io_error("LockFileEx failed: error " + std::to_string(err));
    }

#else
    if (fd_ < 0) {
        if (lock_path_.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(lock_path_.parent_path(), ec);
        }
        fd_ = open(lock_path_.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd_ < 0) {
            return Status::io_error("Failed to open lock file: " + lock_path_.string());
        }
    }

    int op = (mode == ProcessLockMode::Exclusive) ? (LOCK_EX | LOCK_NB) : (LOCK_SH | LOCK_NB);
    int res = flock(fd_, op);
    if (res < 0) {
        if (errno == EACCES || errno == EAGAIN || errno == EWOULDBLOCK) {
            return Status::database_busy("Database lock held by another process: " + lock_path_.string());
        }
        return Status::io_error("flock lock failed with errno " + std::to_string(errno));
    }
#endif

    is_locked_ = true;
    mode_ = mode;
    return Status::ok();
}

Status ProcessFileLock::lock_with_timeout(ProcessLockMode mode, std::chrono::milliseconds timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
        Status st = try_lock(mode);
        if (st.is_ok()) return Status::ok();
        if (st.code() != StatusCode::DatabaseBusy) return st;

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start) >= timeout_ms) {
            return Status::lock_timeout("Timed out waiting for database process lock on " + lock_path_.string());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

Status ProcessFileLock::unlock() {
    if (!is_locked_) {
#ifdef _WIN32
        if (file_handle_) {
            CloseHandle(static_cast<HANDLE>(file_handle_));
            file_handle_ = nullptr;
        }
#else
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
#endif
        return Status::ok();
    }

#ifdef _WIN32
    if (file_handle_) {
        OVERLAPPED ov{};
        UnlockFileEx(static_cast<HANDLE>(file_handle_), 0, 1, 0, &ov);
        CloseHandle(static_cast<HANDLE>(file_handle_));
        file_handle_ = nullptr;
    }
#else
    if (fd_ >= 0) {
        flock(fd_, LOCK_UN);
        close(fd_);
        fd_ = -1;
    }
#endif

    is_locked_ = false;
    return Status::ok();
}

} // namespace klyrdb
