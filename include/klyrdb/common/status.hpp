#pragma once

#include <string>
#include <string_view>

namespace klyrdb {

/// Enumeration of all operational status codes across KlyrDB.
enum class StatusCode {
    Ok = 0,
    InvalidArgument,
    InvalidMagic,
    UnsupportedVersion,
    ChecksumMismatch,
    PageCorrupted,
    PayloadOverflow,
    InvalidPageId,
    FileAlreadyExists,
    FileNotFound,
    IoError,
    TruncatedFile,
    InvalidOffset,
    AlreadyOpen,
    NotOpen,
    BufferPoolFull,
    RecordNotFound,
    InvalidSlotId,
    NoFreeSpace,
    KeyNotFound,
    DuplicateKey,
    IndexCorrupted,
    IndexNotFound,
    KeyTooLarge,
    TableNotFound,
    TableAlreadyExists,
    ColumnNotFound,
    DuplicateColumn,
    TypeMismatch,
    SyntaxError,
    LexerError,
    ConstraintViolation,
    UnsupportedSql,
    WalCorrupted,
    TransactionAborted,
    InvalidTransactionState,
    RecoveryFailed,
    DatabaseBusy,
    LockTimeout,
    InvalidLockState,
    CorruptBackup,
    RestoreFailed,
    ImportError,
    IntegrityCheckFailed,
    Unknown
};

/// Represents the success or failure status of a database engine operation.
class Status {
public:
    constexpr Status() noexcept : code_(StatusCode::Ok), message_() {}
    constexpr explicit Status(StatusCode code) noexcept : code_(code), message_() {}
    explicit Status(StatusCode code, std::string message)
        : code_(code), message_(std::move(message)) {}

    [[nodiscard]] constexpr bool is_ok() const noexcept { return code_ == StatusCode::Ok; }
    [[nodiscard]] constexpr bool is_err() const noexcept { return code_ != StatusCode::Ok; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return is_ok(); }

    [[nodiscard]] constexpr StatusCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] std::string to_string() const;

    // Factory methods for convenient creation
    [[nodiscard]] static constexpr Status ok() noexcept { return Status(StatusCode::Ok); }
    [[nodiscard]] static Status invalid_argument(std::string message) {
        return Status(StatusCode::InvalidArgument, std::move(message));
    }
    [[nodiscard]] static Status invalid_magic(std::string message) {
        return Status(StatusCode::InvalidMagic, std::move(message));
    }
    [[nodiscard]] static Status unsupported_version(std::string message) {
        return Status(StatusCode::UnsupportedVersion, std::move(message));
    }
    [[nodiscard]] static Status checksum_mismatch(std::string message) {
        return Status(StatusCode::ChecksumMismatch, std::move(message));
    }
    [[nodiscard]] static Status page_corrupted(std::string message) {
        return Status(StatusCode::PageCorrupted, std::move(message));
    }
    [[nodiscard]] static Status payload_overflow(std::string message) {
        return Status(StatusCode::PayloadOverflow, std::move(message));
    }
    [[nodiscard]] static Status invalid_page_id(std::string message) {
        return Status(StatusCode::InvalidPageId, std::move(message));
    }
    [[nodiscard]] static Status file_already_exists(std::string message) {
        return Status(StatusCode::FileAlreadyExists, std::move(message));
    }
    [[nodiscard]] static Status file_not_found(std::string message) {
        return Status(StatusCode::FileNotFound, std::move(message));
    }
    [[nodiscard]] static Status io_error(std::string message) {
        return Status(StatusCode::IoError, std::move(message));
    }
    [[nodiscard]] static Status truncated_file(std::string message) {
        return Status(StatusCode::TruncatedFile, std::move(message));
    }
    [[nodiscard]] static Status invalid_offset(std::string message) {
        return Status(StatusCode::InvalidOffset, std::move(message));
    }
    [[nodiscard]] static Status already_open(std::string message) {
        return Status(StatusCode::AlreadyOpen, std::move(message));
    }
    [[nodiscard]] static Status not_open(std::string message) {
        return Status(StatusCode::NotOpen, std::move(message));
    }
    [[nodiscard]] static Status buffer_pool_full(std::string message) {
        return Status(StatusCode::BufferPoolFull, std::move(message));
    }
    [[nodiscard]] static Status record_not_found(std::string message) {
        return Status(StatusCode::RecordNotFound, std::move(message));
    }
    [[nodiscard]] static Status invalid_slot_id(std::string message) {
        return Status(StatusCode::InvalidSlotId, std::move(message));
    }
    [[nodiscard]] static Status no_free_space(std::string message) {
        return Status(StatusCode::NoFreeSpace, std::move(message));
    }
    [[nodiscard]] static Status key_not_found(std::string message) {
        return Status(StatusCode::KeyNotFound, std::move(message));
    }
    [[nodiscard]] static Status duplicate_key(std::string message) {
        return Status(StatusCode::DuplicateKey, std::move(message));
    }
    [[nodiscard]] static Status index_corrupted(std::string message) {
        return Status(StatusCode::IndexCorrupted, std::move(message));
    }
    [[nodiscard]] static Status index_not_found(std::string message) {
        return Status(StatusCode::IndexNotFound, std::move(message));
    }
    [[nodiscard]] static Status key_too_large(std::string message) {
        return Status(StatusCode::KeyTooLarge, std::move(message));
    }
    [[nodiscard]] static Status table_not_found(std::string message) {
        return Status(StatusCode::TableNotFound, std::move(message));
    }
    [[nodiscard]] static Status table_already_exists(std::string message) {
        return Status(StatusCode::TableAlreadyExists, std::move(message));
    }
    [[nodiscard]] static Status column_not_found(std::string message) {
        return Status(StatusCode::ColumnNotFound, std::move(message));
    }
    [[nodiscard]] static Status duplicate_column(std::string message) {
        return Status(StatusCode::DuplicateColumn, std::move(message));
    }
    [[nodiscard]] static Status type_mismatch(std::string message) {
        return Status(StatusCode::TypeMismatch, std::move(message));
    }
    [[nodiscard]] static Status syntax_error(std::string message) {
        return Status(StatusCode::SyntaxError, std::move(message));
    }
    [[nodiscard]] static Status lexer_error(std::string message) {
        return Status(StatusCode::LexerError, std::move(message));
    }
    [[nodiscard]] static Status constraint_violation(std::string message) {
        return Status(StatusCode::ConstraintViolation, std::move(message));
    }
    [[nodiscard]] static Status unsupported_sql(std::string message) {
        return Status(StatusCode::UnsupportedSql, std::move(message));
    }
    [[nodiscard]] static Status wal_corrupted(std::string message) {
        return Status(StatusCode::WalCorrupted, std::move(message));
    }
    [[nodiscard]] static Status transaction_aborted(std::string message) {
        return Status(StatusCode::TransactionAborted, std::move(message));
    }
    [[nodiscard]] static Status invalid_transaction_state(std::string message) {
        return Status(StatusCode::InvalidTransactionState, std::move(message));
    }
    [[nodiscard]] static Status recovery_failed(std::string message) {
        return Status(StatusCode::RecoveryFailed, std::move(message));
    }
    [[nodiscard]] static Status database_busy(std::string message) {
        return Status(StatusCode::DatabaseBusy, std::move(message));
    }
    [[nodiscard]] static Status lock_timeout(std::string message) {
        return Status(StatusCode::LockTimeout, std::move(message));
    }
    [[nodiscard]] static Status invalid_lock_state(std::string message) {
        return Status(StatusCode::InvalidLockState, std::move(message));
    }
    [[nodiscard]] static Status corrupt_backup(std::string message) {
        return Status(StatusCode::CorruptBackup, std::move(message));
    }
    [[nodiscard]] static Status restore_failed(std::string message) {
        return Status(StatusCode::RestoreFailed, std::move(message));
    }
    [[nodiscard]] static Status import_error(std::string message) {
        return Status(StatusCode::ImportError, std::move(message));
    }
    [[nodiscard]] static Status integrity_check_failed(std::string message) {
        return Status(StatusCode::IntegrityCheckFailed, std::move(message));
    }
    [[nodiscard]] static Status unknown(std::string message) {
        return Status(StatusCode::Unknown, std::move(message));
    }

private:
    StatusCode code_;
    std::string message_;
};

[[nodiscard]] std::string_view status_code_to_string(StatusCode code) noexcept;

template <typename OStream>
inline OStream& operator<<(OStream& os, StatusCode code) {
    return os << status_code_to_string(code);
}

} // namespace klyrdb
