#pragma once

#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/common/types.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/concurrency/database_lock.hpp"
#include <chrono>
#include <vector>
#include <memory>

namespace klyrdb {

class TransactionManager;

enum class CheckpointMode {
    Safe,       // Retains all records >= min_required_lsn needed by active txns
    Passive,    // Skips truncation if active transactions exist
    TruncateAll // Only if active_txns is empty
};

struct CheckpointStats {
    lsn_t checkpoint_lsn{0};
    lsn_t min_required_lsn{0};
    size_t active_txns_count{0};
    size_t dirty_pages_flushed{0};
    size_t wal_bytes_before{0};
    size_t wal_bytes_after{0};
    double duration_ms{0.0};
};

/// Dedicated Checkpoint Subsystem coordinating buffer pool flushes, WAL boundary updates,
/// metadata durability, and safe log retention.
class CheckpointManager {
public:
    CheckpointManager(BufferPoolManager& bpm, WriteAheadLog& wal, TransactionManager& txn_mgr) noexcept;
    ~CheckpointManager() = default;

    CheckpointManager(const CheckpointManager&) = delete;
    CheckpointManager& operator=(const CheckpointManager&) = delete;
    CheckpointManager(CheckpointManager&&) = default;
    CheckpointManager& operator=(CheckpointManager&&) = default;

    /// Executes a safe, verifiable checkpoint operation.
    Result<CheckpointStats> checkpoint(CheckpointMode mode = CheckpointMode::Safe);

    /// Checks if a checkpoint is recommended based on WAL file size threshold.
    [[nodiscard]] bool should_checkpoint(size_t wal_size_threshold_bytes) const;

    [[nodiscard]] lsn_t last_checkpoint_lsn() const noexcept { return last_checkpoint_lsn_; }

private:
    BufferPoolManager& bpm_;
    WriteAheadLog& wal_;
    TransactionManager& txn_mgr_;
    std::shared_ptr<DatabaseLock> db_lock_;
    lsn_t last_checkpoint_lsn_{0};
};

} // namespace klyrdb
