#pragma once

#include "klyrdb/concurrency/database_lock.hpp"
#include <unordered_map>
#include <mutex>
#include <memory>
#include <string>

namespace klyrdb {

/// Central coordinator for named database lock instances and concurrency metrics.
class LockManager {
public:
    LockManager() = default;
    ~LockManager() = default;

    LockManager(const LockManager&) = delete;
    LockManager& operator=(const LockManager&) = delete;

    /// Retrieves or instantiates the DatabaseLock for a given database path.
    [[nodiscard]] std::shared_ptr<DatabaseLock> get_lock(const std::filesystem::path& db_path, bool enable_process_lock = true);

    /// Releases any cached lock instance if no threads currently hold it.
    void clean_unused_locks();

    /// Returns the global LockManager singleton.
    static LockManager& instance();

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<DatabaseLock>> locks_;
};

} // namespace klyrdb
