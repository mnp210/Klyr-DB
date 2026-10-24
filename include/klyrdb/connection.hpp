#pragma once

#include "klyrdb/common/result.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/result.hpp"
#include <string_view>
#include <memory>

namespace klyrdb {

class Database;

/// Represents an active connection session to a KlyrDB database.
class Connection {
public:
    explicit Connection(Database& db);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) noexcept;
    Connection& operator=(Connection&&) noexcept;

    /// Executes a single SQL query or mutation.
    [[nodiscard]] Result<ResultSet> execute(std::string_view sql);

    /// Begins an explicit transaction.
    [[nodiscard]] Status begin_transaction();

    /// Commits the active transaction.
    [[nodiscard]] Status commit();

    /// Rolls back the active transaction.
    [[nodiscard]] Status rollback();

    /// Returns true if a transaction is currently in-progress on this connection.
    [[nodiscard]] bool is_in_transaction() const noexcept { return in_transaction_; }

private:
    Database* db_{nullptr};
    bool in_transaction_{false};
};

/// RAII transaction guard that automatically rolls back on destruction unless commit() is called.
class TransactionGuard {
public:
    explicit TransactionGuard(Connection& conn) : conn_(&conn) {
        auto res = conn_->begin_transaction();
        if (res.is_ok()) {
            active_ = true;
        }
    }

    ~TransactionGuard() {
        if (active_ && conn_) {
            (void)conn_->rollback();
        }
    }

    /// Commits the transaction and disarms the automatic rollback guard.
    Status commit() {
        if (!active_ || !conn_) {
            return Status::invalid_argument("Transaction is not active");
        }
        auto res = conn_->commit();
        if (res.is_ok()) {
            active_ = false;
        }
        return res;
    }

    /// Rolls back the transaction and disarms the automatic rollback guard.
    Status rollback() {
        if (!active_ || !conn_) {
            return Status::invalid_argument("Transaction is not active");
        }
        auto res = conn_->rollback();
        if (res.is_ok()) {
            active_ = false;
        }
        return res;
    }

    [[nodiscard]] bool is_active() const noexcept { return active_; }

    TransactionGuard(const TransactionGuard&) = delete;
    TransactionGuard& operator=(const TransactionGuard&) = delete;
    TransactionGuard(TransactionGuard&& other) noexcept
        : conn_(other.conn_), active_(other.active_) {
        other.conn_ = nullptr;
        other.active_ = false;
    }

private:
    Connection* conn_{nullptr};
    bool active_{false};
};

} // namespace klyrdb
