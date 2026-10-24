#include "klyrdb/concurrency/database_lock.hpp"

namespace klyrdb {

DatabaseLock::DatabaseLock(std::filesystem::path db_path, bool enable_process_lock)
    : db_path_(std::move(db_path)),
      enable_process_lock_(enable_process_lock),
      process_lock_(db_path_.string() + ".lock") {}

DatabaseLock::~DatabaseLock() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (exclusive_active_ && exclusive_owner_ == std::this_thread::get_id()) {
        exclusive_active_ = false;
        exclusive_owner_ = std::thread::id{};
        exclusive_recursion_ = 0;
        cv_.notify_all();
    }
    if (enable_process_lock_) {
        (void)process_lock_.unlock();
    }
}

Status DatabaseLock::acquire_shared(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto this_id = std::this_thread::get_id();

    // Reentrant case: if current thread already holds the exclusive lock, it can read safely.
    if (exclusive_active_ && exclusive_owner_ == this_id) {
        return Status::ok();
    }

    auto deadline = std::chrono::steady_clock::now() + timeout;

    while (exclusive_active_ || waiting_writers_ > 0) {
        if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            return Status::lock_timeout("Timed out waiting for shared database lock");
        }
    }

    shared_readers_++;
    return Status::ok();
}

Status DatabaseLock::release_shared() {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto this_id = std::this_thread::get_id();

    // If current thread is the exclusive owner, nothing to release for shared call.
    if (exclusive_active_ && exclusive_owner_ == this_id) {
        return Status::ok();
    }

    if (shared_readers_ == 0) {
        return Status::invalid_lock_state("No shared lock held to release");
    }

    shared_readers_--;
    if (shared_readers_ == 0) {
        cv_.notify_all();
    }
    return Status::ok();
}

Status DatabaseLock::acquire_exclusive(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto this_id = std::this_thread::get_id();

    // Reentrant case: same thread already holds the exclusive lock.
    if (exclusive_active_ && exclusive_owner_ == this_id) {
        exclusive_recursion_++;
        return Status::ok();
    }

    auto deadline = std::chrono::steady_clock::now() + timeout;
    waiting_writers_++;

    while (exclusive_active_ || shared_readers_ > 0) {
        if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            waiting_writers_--;
            cv_.notify_all();
            return Status::lock_timeout("Timed out waiting for exclusive database lock");
        }
    }

    waiting_writers_--;
    exclusive_active_ = true;
    exclusive_owner_ = this_id;
    exclusive_recursion_ = 1;
    return Status::ok();
}

Status DatabaseLock::release_exclusive() {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto this_id = std::this_thread::get_id();

    if (!exclusive_active_ || exclusive_owner_ != this_id) {
        return Status::invalid_lock_state("Thread does not own exclusive database lock");
    }

    exclusive_recursion_--;
    if (exclusive_recursion_ == 0) {
        exclusive_active_ = false;
        exclusive_owner_ = std::thread::id{};
        cv_.notify_all();
    }
    return Status::ok();
}

Status DatabaseLock::acquire_process_lock(ProcessLockMode mode, std::chrono::milliseconds timeout) {
    if (!enable_process_lock_) return Status::ok();
    return process_lock_.lock_with_timeout(mode, timeout);
}

Status DatabaseLock::release_process_lock() {
    if (!enable_process_lock_) return Status::ok();
    return process_lock_.unlock();
}

bool DatabaseLock::is_held_by_current_thread_exclusive() const noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    return exclusive_active_ && exclusive_owner_ == std::this_thread::get_id();
}

bool DatabaseLock::is_exclusively_locked() const noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    return exclusive_active_;
}

size_t DatabaseLock::shared_reader_count() const noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    return shared_readers_;
}

// --- SharedLockGuard ---

Result<SharedLockGuard> SharedLockGuard::acquire(DatabaseLock& lock, std::chrono::milliseconds timeout) {
    Status st = lock.acquire_shared(timeout);
    if (!st.is_ok()) return st;
    return SharedLockGuard(lock);
}

SharedLockGuard::~SharedLockGuard() {
    if (active_ && lock_) {
        (void)lock_->release_shared();
        active_ = false;
    }
}

SharedLockGuard::SharedLockGuard(SharedLockGuard&& other) noexcept
    : lock_(other.lock_), active_(other.active_) {
    other.lock_ = nullptr;
    other.active_ = false;
}

SharedLockGuard& SharedLockGuard::operator=(SharedLockGuard&& other) noexcept {
    if (this != &other) {
        if (active_ && lock_) {
            (void)lock_->release_shared();
        }
        lock_ = other.lock_;
        active_ = other.active_;
        other.lock_ = nullptr;
        other.active_ = false;
    }
    return *this;
}

// --- ExclusiveLockGuard ---

Result<ExclusiveLockGuard> ExclusiveLockGuard::acquire(DatabaseLock& lock, std::chrono::milliseconds timeout) {
    Status st = lock.acquire_exclusive(timeout);
    if (!st.is_ok()) return st;
    return ExclusiveLockGuard(lock);
}

ExclusiveLockGuard::~ExclusiveLockGuard() {
    unlock();
}

void ExclusiveLockGuard::unlock() {
    if (active_ && lock_) {
        (void)lock_->release_exclusive();
        active_ = false;
    }
}

ExclusiveLockGuard::ExclusiveLockGuard(ExclusiveLockGuard&& other) noexcept
    : lock_(other.lock_), active_(other.active_) {
    other.lock_ = nullptr;
    other.active_ = false;
}

ExclusiveLockGuard& ExclusiveLockGuard::operator=(ExclusiveLockGuard&& other) noexcept {
    if (this != &other) {
        unlock();
        lock_ = other.lock_;
        active_ = other.active_;
        other.lock_ = nullptr;
        other.active_ = false;
    }
    return *this;
}

} // namespace klyrdb
