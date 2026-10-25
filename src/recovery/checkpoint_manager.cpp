#include "klyrdb/recovery/checkpoint_manager.hpp"
#include "klyrdb/recovery/crash_simulator.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/storage/metadata_page.hpp"
#include "klyrdb/concurrency/lock_manager.hpp"
#include "klyrdb/diagnostics/metrics.hpp"
#include <chrono>
#include <algorithm>
#include <optional>

namespace klyrdb {

CheckpointManager::CheckpointManager(BufferPoolManager& bpm,
                                     WriteAheadLog& wal,
                                     TransactionManager& txn_mgr) noexcept
    : bpm_(bpm),
      wal_(wal),
      txn_mgr_(txn_mgr),
      db_lock_(LockManager::instance().get_lock(bpm.page_file().path(), false)) {}

bool CheckpointManager::should_checkpoint(size_t wal_size_threshold_bytes) const {
    return wal_.file_size() >= wal_size_threshold_bytes;
}

Result<CheckpointStats> CheckpointManager::checkpoint(CheckpointMode mode) {
    auto start_time = std::chrono::steady_clock::now();
    CrashSimulator::trigger(CrashPoint::BeforeCheckpoint);

    // 1. Acquire exclusive lock to prevent concurrent modifications
    std::optional<ExclusiveLockGuard> excl_guard;
    if (db_lock_) {
        auto g_res = ExclusiveLockGuard::acquire(*db_lock_);
        if (g_res.is_err()) {
            return g_res.status();
        }
        excl_guard = std::move(g_res.value());
    }

    CheckpointStats stats;
    stats.wal_bytes_before = wal_.file_size();

    // 2. Determine active transactions and minimum required LSN
    auto active_txns = txn_mgr_.active_transactions();
    stats.active_txns_count = active_txns.size();

    lsn_t min_required_lsn = wal_.last_lsn() + 1;
    std::vector<txn_id_t> active_txn_ids;

    if (!active_txns.empty()) {
        lsn_t lowest_active = INVALID_LSN;
        for (const auto& txn : active_txns) {
            active_txn_ids.push_back(txn->txn_id());
            lsn_t f_lsn = txn->first_lsn();
            if (f_lsn != INVALID_LSN) {
                if (lowest_active == INVALID_LSN || f_lsn < lowest_active) {
                    lowest_active = f_lsn;
                }
            }
        }
        if (lowest_active != INVALID_LSN) {
            min_required_lsn = lowest_active;
        }
    }
    stats.min_required_lsn = min_required_lsn;

    // 3. WAL-Before-Data enforcement: ensure all logged modifications are durable
    Status wal_flush_st = wal_.flush();
    if (!wal_flush_st.is_ok()) {
        return wal_flush_st;
    }

    // 4. Flush all dirty cached pages to disk and sync database file
    Status bpm_flush_st = bpm_.flush_all_pages();
    if (!bpm_flush_st.is_ok()) {
        return bpm_flush_st;
    }

    Status sync_st = bpm_.page_file().sync();
    if (!sync_st.is_ok()) {
        return sync_st;
    }

    CrashSimulator::trigger(CrashPoint::AfterPagesFlushedBeforeCheckpointDurable);

    // 5. Append Checkpoint record with active transactions table
    lsn_t current_last_lsn = wal_.last_lsn();
    LogRecord chk_rec = LogRecord::make_checkpoint(
        INVALID_LSN,
        current_last_lsn,
        min_required_lsn,
        active_txn_ids
    );

    auto append_res = wal_.append_record(chk_rec);
    if (append_res.is_err()) {
        return append_res.status();
    }
    lsn_t chk_record_lsn = append_res.value();
    stats.checkpoint_lsn = chk_record_lsn;
    last_checkpoint_lsn_ = chk_record_lsn;

    CrashSimulator::trigger(CrashPoint::AfterCheckpointAppendedBeforeDurable);

    // Flush WAL containing the checkpoint record
    Status chk_wal_flush = wal_.flush();
    if (!chk_wal_flush.is_ok()) {
        return chk_wal_flush;
    }

    // 6. Update checkpoint metadata on WAL header and Page 0
    Status wal_hdr_st = wal_.update_checkpoint_boundary(chk_record_lsn);
    if (!wal_hdr_st.is_ok()) {
        return wal_hdr_st;
    }

    // Persist checkpoint LSN into Page 0 DatabaseMetadata
    {
        auto p0_res = bpm_.fetch_page_guard(0);
        if (p0_res.is_ok()) {
            auto& p0_guard = p0_res.value();
            auto meta_res = DatabaseMetadata::decode_from_page(*p0_guard.get());
            if (meta_res.is_ok()) {
                auto meta = meta_res.value();
                meta.checkpoint_lsn = chk_record_lsn;
                (void)meta.encode_to_page(*p0_guard.get());
                p0_guard.mark_dirty();
            }
        }
        (void)bpm_.flush_page(0);
        (void)bpm_.page_file().sync();
    }

    CrashSimulator::trigger(CrashPoint::AfterMetadataDurableBeforeTruncation);

    // 7. Safe WAL truncation / retention
    if (mode == CheckpointMode::Safe) {
        // If no active transactions, truncate everything prior to checkpoint record
        // If active transactions exist, preserve records >= min_required_lsn
        Status trunc_st = wal_.truncate_safe(chk_record_lsn, min_required_lsn);
        if (!trunc_st.is_ok()) {
            return trunc_st;
        }
    } else if (mode == CheckpointMode::TruncateAll) {
        if (active_txns.empty()) {
            Status trunc_st = wal_.truncate_safe(chk_record_lsn, INVALID_LSN);
            if (!trunc_st.is_ok()) {
                return trunc_st;
            }
        }
    }
    // If mode == CheckpointMode::Passive and active_txns is not empty, truncation is skipped

    stats.wal_bytes_after = wal_.file_size();
    auto end_time = std::chrono::steady_clock::now();
    stats.duration_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    uint64_t dur_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();

    if (bpm_.metrics() != nullptr) {
        bpm_.metrics()->record_checkpoint(true, dur_us);
    }

    return stats;
}

} // namespace klyrdb
