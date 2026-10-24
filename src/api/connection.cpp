#include "klyrdb/connection.hpp"
#include "klyrdb/database.hpp"

namespace klyrdb {

Connection::Connection(Database& db) : db_(&db), in_transaction_(false) {}

Connection::~Connection() {
    if (in_transaction_ && db_ && db_->is_open()) {
        (void)rollback();
    }
}

Connection::Connection(Connection&& other) noexcept
    : db_(other.db_), in_transaction_(other.in_transaction_) {
    other.db_ = nullptr;
    other.in_transaction_ = false;
}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        if (in_transaction_ && db_ && db_->is_open()) {
            (void)rollback();
        }
        db_ = other.db_;
        in_transaction_ = other.in_transaction_;
        other.db_ = nullptr;
        other.in_transaction_ = false;
    }
    return *this;
}

Result<ResultSet> Connection::execute(std::string_view sql) {
    if (!db_ || !db_->is_open()) {
        return Status::io_error("Database is closed or connection is invalid");
    }
    return db_->execute(sql);
}

Status Connection::begin_transaction() {
    if (!db_ || !db_->is_open()) {
        return Status::io_error("Database is closed");
    }
    if (in_transaction_) {
        return Status::invalid_argument("Transaction already active on this connection");
    }
    auto res = db_->execute("BEGIN;");
    if (!res.is_ok()) {
        return res.status();
    }
    in_transaction_ = true;
    return Status::ok();
}

Status Connection::commit() {
    if (!db_ || !db_->is_open()) {
        return Status::io_error("Database is closed");
    }
    if (!in_transaction_) {
        return Status::invalid_argument("No active transaction to commit");
    }
    auto res = db_->execute("COMMIT;");
    if (!res.is_ok()) {
        return res.status();
    }
    in_transaction_ = false;
    return Status::ok();
}

Status Connection::rollback() {
    if (!db_ || !db_->is_open()) {
        return Status::io_error("Database is closed");
    }
    if (!in_transaction_) {
        return Status::invalid_argument("No active transaction to roll back");
    }
    auto res = db_->execute("ROLLBACK;");
    in_transaction_ = false;
    if (!res.is_ok()) {
        return res.status();
    }
    return Status::ok();
}

} // namespace klyrdb
