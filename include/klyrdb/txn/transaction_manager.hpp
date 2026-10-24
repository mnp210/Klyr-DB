#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction.hpp"
#include <memory>
#include <unordered_map>
#include <mutex>

namespace klyrdb {

/// Coordinates transaction lifecycle (BEGIN, COMMIT, ROLLBACK), WAL logging,
/// and undo-rollback page restorations.
class TransactionManager {
public:
    TransactionManager(BufferPoolManager& bpm, WriteAheadLog& wal) noexcept;
    ~TransactionManager();

    /// Begins a new transaction, emitting a BEGIN log record.
    Result<std::shared_ptr<Transaction>> begin();

    /// Commits an active transaction, emitting a COMMIT log record and durably flushing the WAL.
    Status commit(std::shared_ptr<Transaction> txn);

    /// Rolls back an active transaction, restoring modified pages in reverse order from before-images,
    /// and emitting an ABORT log record.
    Status rollback(std::shared_ptr<Transaction> txn);

    /// Logs a page update with before-image and after-image, setting the page LSN.
    Result<lsn_t> log_page_update(std::shared_ptr<Transaction> txn,
                                  page_id_t page_id,
                                  const Page& before_image,
                                  Page& modified_page);

    /// Creates a checkpoint: flushes all dirty buffer pool pages, records checkpoint,
    /// and safely truncates the WAL file.
    Status checkpoint();

    /// Current explicit transaction (if in BEGIN block).
    [[nodiscard]] std::shared_ptr<Transaction> current_transaction() const noexcept {
        std::lock_guard<std::recursive_mutex> lock(txn_mutex_);
        return current_txn_;
    }
    void set_current_transaction(std::shared_ptr<Transaction> txn) noexcept {
        std::lock_guard<std::recursive_mutex> lock(txn_mutex_);
        current_txn_ = std::move(txn);
    }

    [[nodiscard]] BufferPoolManager& buffer_pool_manager() noexcept { return bpm_; }
    [[nodiscard]] WriteAheadLog& wal() noexcept { return wal_; }

    [[nodiscard]] size_t active_transaction_count() const noexcept {
        std::lock_guard<std::recursive_mutex> lock(txn_mutex_);
        return active_txns_.size();
    }

    [[nodiscard]] std::vector<std::shared_ptr<Transaction>> active_transactions() const {
        std::lock_guard<std::recursive_mutex> lock(txn_mutex_);
        std::vector<std::shared_ptr<Transaction>> list;
        list.reserve(active_txns_.size());
        for (const auto& [id, txn] : active_txns_) {
            list.push_back(txn);
        }
        return list;
    }

private:
    mutable std::recursive_mutex txn_mutex_;
    BufferPoolManager& bpm_;
    WriteAheadLog& wal_;
    txn_id_t last_txn_id_{0};
    std::unordered_map<txn_id_t, std::shared_ptr<Transaction>> active_txns_;
    std::shared_ptr<Transaction> current_txn_{nullptr};
};

} // namespace klyrdb
