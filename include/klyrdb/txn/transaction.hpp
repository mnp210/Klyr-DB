#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/storage/page.hpp"
#include "klyrdb/wal/log_record.hpp"
#include <vector>
#include <string_view>

namespace klyrdb {

enum class TransactionState {
    Active,
    Committed,
    Aborted
};

[[nodiscard]] constexpr std::string_view to_string(TransactionState state) noexcept {
    switch (state) {
        case TransactionState::Active:    return "Active";
        case TransactionState::Committed: return "Committed";
        case TransactionState::Aborted:   return "Aborted";
        default:                          return "Unknown";
    }
}

template <typename OStream>
inline OStream& operator<<(OStream& os, TransactionState state) {
    return os << to_string(state);
}

struct PageChange {
    page_id_t page_id{INVALID_PAGE_ID};
    Page before_image;
    lsn_t lsn{INVALID_LSN};
};

/// Represents an in-flight or completed transaction in KlyrDB.
class Transaction {
public:
    explicit Transaction(txn_id_t txn_id) noexcept
        : txn_id_(txn_id), state_(TransactionState::Active) {}

    [[nodiscard]] txn_id_t txn_id() const noexcept { return txn_id_; }
    [[nodiscard]] TransactionState state() const noexcept { return state_; }
    [[nodiscard]] bool is_active() const noexcept { return state_ == TransactionState::Active; }
    [[nodiscard]] bool is_committed() const noexcept { return state_ == TransactionState::Committed; }
    [[nodiscard]] bool is_aborted() const noexcept { return state_ == TransactionState::Aborted; }
    [[nodiscard]] bool is_rolling_back() const noexcept { return is_rolling_back_; }
    void set_rolling_back(bool flag) noexcept { is_rolling_back_ = flag; }

    [[nodiscard]] lsn_t first_lsn() const noexcept { return first_lsn_; }
    void set_first_lsn(lsn_t lsn) noexcept { if (first_lsn_ == INVALID_LSN) first_lsn_ = lsn; }

    [[nodiscard]] lsn_t prev_lsn() const noexcept { return prev_lsn_; }
    void set_prev_lsn(lsn_t lsn) noexcept { prev_lsn_ = lsn; }

    void set_state(TransactionState state) noexcept { state_ = state; }

    void record_page_change(page_id_t page_id, Page before_image, lsn_t lsn) {
        page_changes_.push_back(PageChange{page_id, std::move(before_image), lsn});
    }

    [[nodiscard]] const std::vector<PageChange>& page_changes() const noexcept {
        return page_changes_;
    }

private:
    txn_id_t txn_id_{INVALID_TXN_ID};
    TransactionState state_{TransactionState::Active};
    lsn_t first_lsn_{INVALID_LSN};
    lsn_t prev_lsn_{INVALID_LSN};
    bool is_rolling_back_{false};
    std::vector<PageChange> page_changes_;
};

} // namespace klyrdb
