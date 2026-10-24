#pragma once

#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include <unordered_set>
#include <vector>

namespace klyrdb {

struct RecoveryStats {
    size_t total_log_records{0};
    size_t records_scanned{0};
    size_t committed_txns{0};
    size_t uncommitted_txns{0};
    size_t pages_redone{0};
    size_t pages_undone{0};
    lsn_t checkpoint_lsn{0};
    double duration_ms{0.0};
    size_t wal_size_before{0};
    size_t wal_size_after{0};
};

/// Crash Recovery Manager executing Analysis, Redo, and Undo passes over the WAL.
class RecoveryManager {
public:
    RecoveryManager(BufferPoolManager& bpm, WriteAheadLog& wal) noexcept
        : bpm_(bpm), wal_(wal) {}

    /// Checks if the WAL contains pending uncheckpointed records needing recovery.
    [[nodiscard]] bool needs_recovery();

    /// Performs restartable crash recovery (Analysis -> Redo -> Undo -> Flush).
    Result<RecoveryStats> recover();

private:
    BufferPoolManager& bpm_;
    WriteAheadLog& wal_;
};

} // namespace klyrdb
