#include "klyrdb/common/status.hpp"

namespace klyrdb {

std::string_view status_code_to_string(StatusCode code) noexcept {
    switch (code) {
        case StatusCode::Ok:                 return "Ok";
        case StatusCode::InvalidArgument:    return "InvalidArgument";
        case StatusCode::InvalidMagic:       return "InvalidMagic";
        case StatusCode::UnsupportedVersion: return "UnsupportedVersion";
        case StatusCode::ChecksumMismatch:   return "ChecksumMismatch";
        case StatusCode::PageCorrupted:      return "PageCorrupted";
        case StatusCode::PayloadOverflow:    return "PayloadOverflow";
        case StatusCode::InvalidPageId:      return "InvalidPageId";
        case StatusCode::FileAlreadyExists:  return "FileAlreadyExists";
        case StatusCode::FileNotFound:       return "FileNotFound";
        case StatusCode::IoError:            return "IoError";
        case StatusCode::TruncatedFile:      return "TruncatedFile";
        case StatusCode::InvalidOffset:      return "InvalidOffset";
        case StatusCode::AlreadyOpen:        return "AlreadyOpen";
        case StatusCode::NotOpen:            return "NotOpen";
        case StatusCode::BufferPoolFull:     return "BufferPoolFull";
        case StatusCode::RecordNotFound:     return "RecordNotFound";
        case StatusCode::InvalidSlotId:      return "InvalidSlotId";
        case StatusCode::NoFreeSpace:        return "NoFreeSpace";
        case StatusCode::KeyNotFound:        return "KeyNotFound";
        case StatusCode::DuplicateKey:       return "DuplicateKey";
        case StatusCode::IndexCorrupted:     return "IndexCorrupted";
        case StatusCode::IndexNotFound:      return "IndexNotFound";
        case StatusCode::KeyTooLarge:        return "KeyTooLarge";
        case StatusCode::TableNotFound:      return "TableNotFound";
        case StatusCode::TableAlreadyExists:  return "TableAlreadyExists";
        case StatusCode::ColumnNotFound:     return "ColumnNotFound";
        case StatusCode::DuplicateColumn:    return "DuplicateColumn";
        case StatusCode::TypeMismatch:       return "TypeMismatch";
        case StatusCode::SyntaxError:        return "SyntaxError";
        case StatusCode::LexerError:         return "LexerError";
        case StatusCode::ConstraintViolation: return "ConstraintViolation";
        case StatusCode::UnsupportedSql:     return "UnsupportedSql";
        case StatusCode::WalCorrupted:        return "WalCorrupted";
        case StatusCode::TransactionAborted:  return "TransactionAborted";
        case StatusCode::InvalidTransactionState: return "InvalidTransactionState";
        case StatusCode::RecoveryFailed:      return "RecoveryFailed";
        case StatusCode::DatabaseBusy:        return "DatabaseBusy";
        case StatusCode::LockTimeout:         return "LockTimeout";
        case StatusCode::InvalidLockState:    return "InvalidLockState";
        case StatusCode::CorruptBackup:       return "CorruptBackup";
        case StatusCode::RestoreFailed:       return "RestoreFailed";
        case StatusCode::ImportError:         return "ImportError";
        case StatusCode::IntegrityCheckFailed: return "IntegrityCheckFailed";
        case StatusCode::Unknown:
        default:                             return "Unknown";
    }
}

std::string Status::to_string() const {
    if (code_ == StatusCode::Ok) {
        return "Ok";
    }
    std::string s(status_code_to_string(code_));
    if (!message_.empty()) {
        s += ": ";
        s += message_;
    }
    return s;
}

} // namespace klyrdb
