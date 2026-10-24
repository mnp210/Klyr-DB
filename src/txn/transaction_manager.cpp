#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/recovery/crash_simulator.hpp"
#include "klyrdb/recovery/checkpoint_manager.hpp"
#include "klyrdb/diagnostics/metrics.hpp"
#include <cstring>

namespace klyrdb {

TransactionManager::TransactionManager(BufferPoolManager& bpm, WriteAheadLog& wal) noexcept
    : bpm_(bpm), wal_(wal) {
    bpm_.set_wal(&wal_);
    bpm_.set_transaction_manager(this);

    // Initialize last_txn_id_ by scanning existing records in the WAL
    auto records_res = wal_.read_all_records();
    if (records_res.is_ok()) {
        txn_id_t max_tid = 0;
        for (const auto& rec : records_res.value()) {
            if (rec.txn_id > max_tid) {
                max_tid = rec.txn_id;
            }
        }
        last_txn_id_ = max_tid;
    }
}

TransactionManager::~TransactionManager() {
    bpm_.set_transaction_manager(nullptr);
    bpm_.set_wal(nullptr);
}

Result<std::shared_ptr<Transaction>> TransactionManager::begin() {
    std::lock_guard<std::recursive_mutex> lock(txn_mutex_);
    auto txn = std::make_shared<Transaction>(++last_txn_id_);

    LogRecord begin_rec = LogRecord::make_begin(INVALID_LSN, txn->txn_id());
    auto lsn_res = wal_.append_record(begin_rec);
    if (lsn_res.is_err()) {
        return lsn_res.status();
    }

    txn->set_first_lsn(lsn_res.value());
    txn->set_prev_lsn(lsn_res.value());
    active_txns_[txn->txn_id()] = txn;

    if (bpm_.metrics() != nullptr) {
        bpm_.metrics()->record_transaction_started();
    }

    return txn;
}

Status TransactionManager::commit(std::shared_ptr<Transaction> txn) {
    std::lock_guard<std::recursive_mutex> lock(txn_mutex_);
    if (!txn) {
        return Status::invalid_argument("Transaction pointer is null");
    }
    if (!txn->is_active()) {
        return Status::invalid_transaction_state("Cannot commit transaction in state: " +
                                                std::string(to_string(txn->state())));
    }

    LogRecord commit_rec = LogRecord::make_commit(INVALID_LSN, txn->txn_id(), txn->prev_lsn());
    auto lsn_res = wal_.append_record(commit_rec);
    if (lsn_res.is_err()) {
        if (bpm_.metrics() != nullptr) {
            bpm_.metrics()->record_commit_failure();
        }
        return lsn_res.status();
    }

    // Durably flush WAL to disk before confirming commit
    Status flush_st = wal_.flush();
    if (!flush_st.is_ok()) {
        if (bpm_.metrics() != nullptr) {
            bpm_.metrics()->record_commit_failure();
        }
        return flush_st;
    }

    CrashSimulator::trigger(CrashPoint::AfterCommitDurableBeforeSuccess);

    txn->set_state(TransactionState::Committed);
    active_txns_.erase(txn->txn_id());

    if (current_txn_ == txn) {
        current_txn_ = nullptr;
    }

    if (bpm_.metrics() != nullptr) {
        bpm_.metrics()->record_transaction_committed();
    }

    return Status::ok();
}

Status TransactionManager::rollback(std::shared_ptr<Transaction> txn) {
    std::lock_guard<std::recursive_mutex> lock(txn_mutex_);
    if (!txn) {
        return Status::invalid_argument("Transaction pointer is null");
    }
    if (!txn->is_active()) {
        return Status::invalid_transaction_state("Cannot rollback transaction in state: " +
                                                std::string(to_string(txn->state())));
    }

    txn->set_rolling_back(true);
    const auto& changes = txn->page_changes();

    // Undo modifications in reverse order
    for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
        CrashSimulator::trigger(CrashPoint::DuringRollback);
        auto guard_res = bpm_.fetch_page_guard(it->page_id);
        if (guard_res.is_err()) {
            txn->set_rolling_back(false);
            return guard_res.status();
        }

        auto& guard = guard_res.value();
        // Restore full before-image
        guard->load_from_raw(it->before_image.raw_data());
        guard->set_page_lsn(it->before_image.page_lsn());
        guard.mark_dirty();
    }
    txn->set_rolling_back(false);

    LogRecord abort_rec = LogRecord::make_abort(INVALID_LSN, txn->txn_id(), txn->prev_lsn());
    auto lsn_res = wal_.append_record(abort_rec);
    if (lsn_res.is_err()) {
        return lsn_res.status();
    }

    Status flush_st = wal_.flush();
    if (!flush_st.is_ok()) {
        return flush_st;
    }

    txn->set_state(TransactionState::Aborted);
    active_txns_.erase(txn->txn_id());

    if (current_txn_ == txn) {
        current_txn_ = nullptr;
    }

    if (bpm_.metrics() != nullptr) {
        bpm_.metrics()->record_transaction_aborted();
        bpm_.metrics()->record_rollback();
    }

    return Status::ok();
}

Result<lsn_t> TransactionManager::log_page_update(std::shared_ptr<Transaction> txn,
                                                  page_id_t page_id,
                                                  const Page& before_image,
                                                  Page& modified_page) {
    std::lock_guard<std::recursive_mutex> lock(txn_mutex_);
    if (!txn || !txn->is_active()) {
        return Status::invalid_transaction_state("Transaction is not active");
    }

    CrashSimulator::trigger(CrashPoint::BeforePageModify);

    LogRecord rec = LogRecord::make_page_update(
        INVALID_LSN,
        txn->txn_id(),
        txn->prev_lsn(),
        page_id,
        before_image.raw_data(),
        modified_page.raw_data()
    );

    auto lsn_res = wal_.append_record(rec);
    if (lsn_res.is_err()) {
        return lsn_res.status();
    }

    CrashSimulator::trigger(CrashPoint::AfterLogBeforePageFlush);

    lsn_t new_lsn = lsn_res.value();
    txn->set_prev_lsn(new_lsn);
    modified_page.set_page_lsn(static_cast<uint32_t>(new_lsn));
    txn->record_page_change(page_id, before_image, new_lsn);

    CrashSimulator::trigger(CrashPoint::AfterMemoryModify);

    return new_lsn;
}

Status TransactionManager::checkpoint() {
    CheckpointManager chk_mgr(bpm_, wal_, *this);
    auto res = chk_mgr.checkpoint(CheckpointMode::Safe);
    return res.status();
}

} // namespace klyrdb
