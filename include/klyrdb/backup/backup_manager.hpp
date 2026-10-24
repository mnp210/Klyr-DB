#pragma once

#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/backup/backup_manifest.hpp"
#include <filesystem>
#include <chrono>

namespace klyrdb {

struct BackupOptions {
    bool overwrite{false};
    bool verify_after_backup{true};
    std::chrono::milliseconds lock_timeout{5000};
};

struct BackupResult {
    std::filesystem::path backup_path;
    BackupManifest manifest;
    double duration_ms{0.0};
    uint64_t total_backup_bytes{0};
};

/// High-level physical backup engine coordinating snapshot locks, buffer flushes,
/// checkpoint boundary synchronization, component checksumming, and atomic publication.
class BackupManager {
public:
    BackupManager() = default;
    ~BackupManager() = default;

    /// Creates a complete, self-contained, and verified physical backup.
    Result<BackupResult> create_backup(
        const std::filesystem::path& source_db_path,
        const std::filesystem::path& backup_path,
        const BackupOptions& options = {}) const;
};

} // namespace klyrdb
