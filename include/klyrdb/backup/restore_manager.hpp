#pragma once

#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include <filesystem>
#include <chrono>

namespace klyrdb {

struct RestoreOptions {
    bool force_overwrite{false};
    bool verify_before_restore{true};
    bool run_recovery{true};
    bool verify_after_restore{true};
    std::chrono::milliseconds lock_timeout{5000};
};

struct RestoreResult {
    std::filesystem::path dest_db_path;
    double duration_ms{0.0};
    size_t pages_restored{0};
    std::string message;
};

/// Disaster-recovery and restore engine validating backup integrity,
/// staging non-destructively, applying crash recovery, and publishing atomically.
class RestoreManager {
public:
    RestoreManager() = default;
    ~RestoreManager() = default;

    /// Restores a physical backup into the target database path.
    Result<RestoreResult> restore(
        const std::filesystem::path& backup_path,
        const std::filesystem::path& dest_db_path,
        const RestoreOptions& options = {}) const;
};

} // namespace klyrdb
