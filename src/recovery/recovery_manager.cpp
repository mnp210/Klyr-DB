#include "klyrdb/recovery/recovery_manager.hpp"
#include "klyrdb/recovery/crash_simulator.hpp"
#include "klyrdb/storage/metadata_page.hpp"
#include "klyrdb/concurrency/lock_manager.hpp"
#include "klyrdb/diagnostics/metrics.hpp"
#include "klyrdb/common/endian.hpp"
#include <unordered_set>
#include <cstring>
#include <chrono>

namespace klyrdb {

bool RecoveryManager::needs_recovery() {
    if (!wal_.is_open()) {
        return false;
    }
    auto recs_res = wal_.read_all_records();
    if (recs_res.is_err()) {
        return false;
    }
    const auto& records = recs_res.value();
    if (records.empty()) {
        return false;
    }

    lsn_t last_ckpt = 0;
    std::unordered_set<txn_id_t> committed;
    std::unordered_set<txn_id_t> aborted;
    std::unordered_set<txn_id_t> active;

    for (const auto& rec : records) {
        if (rec.type == LogRecordType::Checkpoint) {
            last_ckpt = rec.lsn;
            active.clear();
        } else if (rec.type == LogRecordType::Begin) {
            active.insert(rec.txn_id);
        } else if (rec.type == LogRecordType::Commit) {
            committed.insert(rec.txn_id);
            active.erase(rec.txn_id);
        } else if (rec.type == LogRecordType::Abort) {
            aborted.insert(rec.txn_id);
            active.erase(rec.txn_id);
        } else if (rec.type == LogRecordType::PageUpdate ||
                   rec.type == LogRecordType::PageAlloc) {
            if (rec.txn_id != 0 && committed.find(rec.txn_id) == committed.end()) {
                active.insert(rec.txn_id);
            }
        }
    }

    return (records.back().lsn > last_ckpt) || !active.empty();
}

Result<RecoveryStats> RecoveryManager::recover() {
    auto start_time = std::chrono::steady_clock::now();
    CrashSimulator::trigger(CrashPoint::DuringRecovery);

    auto dblock = LockManager::instance().get_lock(bpm_.page_file().path(), false);
    auto excl_guard = ExclusiveLockGuard::acquire(*dblock);
    if (excl_guard.is_err()) {
        return excl_guard.status();
    }

    RecoveryStats stats;
    stats.wal_size_before = wal_.file_size();

    if (!wal_.is_open()) {
        return stats;
    }

    auto recs_res = wal_.read_all_records();
    if (recs_res.is_err()) {
        return recs_res.status();
    }

    const auto& records = recs_res.value();
    stats.total_log_records = records.size();

    if (records.empty()) {
        stats.wal_size_after = wal_.file_size();
        return stats;
    }

    // Check Page 0 metadata for expected checkpoint boundary
    Page p0_check;
    if (bpm_.page_file().read_page(0, p0_check).is_ok()) {
        auto meta_res = DatabaseMetadata::decode_from_page(p0_check);
        if (meta_res.is_ok()) {
            lsn_t p0_chk = meta_res.value().checkpoint_lsn;
            if (p0_chk > 0 && !records.empty() && records.back().lsn < p0_chk) {
                return Status::wal_corrupted("WAL is missing log records required by database checkpoint LSN " +
                                            std::to_string(p0_chk));
            }
        }
    }

    // --- Phase 1: Analysis Pass ---
    CrashSimulator::trigger(CrashPoint::DuringRecoveryAnalysis);

    std::unordered_set<txn_id_t> committed_txns;
    std::unordered_set<txn_id_t> aborted_txns;
    std::unordered_set<txn_id_t> all_txns;
    lsn_t last_checkpoint_lsn = 0;
    lsn_t min_required_lsn = 0;

    for (const auto& rec : records) {
        if (rec.type == LogRecordType::Checkpoint) {
            last_checkpoint_lsn = rec.lsn;
            stats.checkpoint_lsn = rec.lsn;
            if (rec.before_image.size() >= 12) {
                min_required_lsn = decode_u64_be(rec.before_image.data());
            } else {
                min_required_lsn = rec.lsn;
            }
        } else if (rec.type == LogRecordType::Commit) {
            committed_txns.insert(rec.txn_id);
        } else if (rec.type == LogRecordType::Abort) {
            aborted_txns.insert(rec.txn_id);
        } else if (rec.type == LogRecordType::Begin ||
                   rec.type == LogRecordType::PageUpdate ||
                   rec.type == LogRecordType::PageAlloc) {
            if (rec.txn_id != 0) {
                all_txns.insert(rec.txn_id);
            }
        }
    }

    std::unordered_set<txn_id_t> txns_to_undo;
    for (txn_id_t tid : all_txns) {
        if (committed_txns.find(tid) == committed_txns.end() &&
            aborted_txns.find(tid) == aborted_txns.end()) {
            txns_to_undo.insert(tid);
        }
    }

    stats.committed_txns = committed_txns.size();
    stats.uncommitted_txns = txns_to_undo.size();

    // Determine safe recovery scan start point
    lsn_t scan_start_lsn = (min_required_lsn > 0) ? min_required_lsn : last_checkpoint_lsn;

    size_t scanned = 0;
    for (const auto& rec : records) {
        if (rec.lsn >= scan_start_lsn) {
            scanned++;
        }
    }
    stats.records_scanned = scanned;

    // --- Phase 2: Redo Pass (Forward Scan) ---
    CrashSimulator::trigger(CrashPoint::DuringRecoveryRedo);

    for (const auto& rec : records) {
        if (rec.lsn < scan_start_lsn) {
            continue;
        }

        if (committed_txns.find(rec.txn_id) != committed_txns.end()) {
            if (rec.type == LogRecordType::PageUpdate ||
                rec.type == LogRecordType::PageAlloc) {
                
                // Ensure page exists in underlying PageFile
                while (bpm_.page_file().page_count() <= rec.page_id) {
                    Page dummy;
                    auto append_res = bpm_.page_file().append_page(dummy);
                    if (append_res.is_err()) {
                        return append_res.status();
                    }
                }

                auto guard_res = bpm_.fetch_page_guard(rec.page_id);
                if (guard_res.is_err()) {
                    return guard_res.status();
                }

                auto& guard = guard_res.value();
                if (guard->page_lsn() < rec.lsn) {
                    if (rec.after_image.size() == PAGE_SIZE) {
                        guard->load_from_raw(std::span<const uint8_t, PAGE_SIZE>(rec.after_image.data(), PAGE_SIZE));
                        guard->set_page_lsn(static_cast<uint32_t>(rec.lsn));
                        guard.mark_dirty();
                        stats.pages_redone++;
                    }
                }
            }
        }
    }

    // --- Phase 3: Undo Pass (Reverse Scan) ---
    CrashSimulator::trigger(CrashPoint::DuringRecoveryUndo);

    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        const auto& rec = *it;
        if (txns_to_undo.find(rec.txn_id) != txns_to_undo.end()) {
            if (rec.type == LogRecordType::PageUpdate ||
                rec.type == LogRecordType::PageAlloc) {
                
                if (rec.page_id < bpm_.page_file().page_count()) {
                    auto guard_res = bpm_.fetch_page_guard(rec.page_id);
                    if (guard_res.is_ok()) {
                        auto& guard = guard_res.value();
                        if (guard->page_lsn() >= rec.lsn) {
                            if (rec.before_image.size() == PAGE_SIZE) {
                                guard->load_from_raw(std::span<const uint8_t, PAGE_SIZE>(rec.before_image.data(), PAGE_SIZE));
                                guard->set_page_lsn(static_cast<uint32_t>(rec.prev_lsn));
                                guard.mark_dirty();
                                stats.pages_undone++;
                            }
                        }
                    }
                }
            }
        }
    }

    // --- Phase 4: Flush and Sync ---
    Status flush_st = bpm_.flush_all_pages();
    if (flush_st.is_err()) {
        return flush_st;
    }
    (void)bpm_.page_file().sync();

    // Reload Page 0 metadata in PageFile if Page 0 was recovered
    Page p0;
    if (bpm_.page_file().read_page(0, p0).is_ok()) {
        auto meta_res = DatabaseMetadata::decode_from_page(p0);
        if (meta_res.is_ok()) {
            (void)bpm_.page_file().update_metadata(meta_res.value());
        }
    }

    // Append Abort records for all undone transactions to mark them durably resolved
    for (txn_id_t tid : txns_to_undo) {
        LogRecord abort_rec = LogRecord::make_abort(INVALID_LSN, tid, wal_.last_lsn());
        (void)wal_.append_record(abort_rec);
    }

    // Append Checkpoint record and flush WAL
    lsn_t chk_lsn = wal_.last_lsn();
    LogRecord chk_rec = LogRecord::make_checkpoint(INVALID_LSN, chk_lsn);
    auto append_res = wal_.append_record(chk_rec);
    if (append_res.is_ok()) {
        (void)wal_.flush();
        (void)wal_.update_checkpoint_boundary(append_res.value());

        // Ensure Page 0 metadata checkpoint LSN is synchronized with the new checkpoint
        auto p0_res = bpm_.fetch_page_guard(0);
        if (p0_res.is_ok()) {
            auto& p0_guard = p0_res.value();
            auto meta_res = DatabaseMetadata::decode_from_page(*p0_guard.get());
            if (meta_res.is_ok()) {
                auto meta = meta_res.value();
                meta.checkpoint_lsn = append_res.value();
                (void)meta.encode_to_page(*p0_guard.get());
                p0_guard.mark_dirty();
                (void)bpm_.page_file().update_metadata(meta);
            }
        }
        (void)bpm_.flush_page(0);
        (void)bpm_.page_file().sync();
    }

    stats.wal_size_after = wal_.file_size();
    auto end_time = std::chrono::steady_clock::now();
    stats.duration_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    uint64_t dur_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();

    if (bpm_.metrics() != nullptr) {
        bpm_.metrics()->record_recovery(true, dur_us, stats.pages_redone, stats.pages_undone);
    }

    return stats;
}

} // namespace klyrdb
