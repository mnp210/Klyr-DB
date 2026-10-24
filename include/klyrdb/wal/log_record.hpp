#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/storage/page_constants.hpp"
#include <cstdint>
#include <vector>
#include <span>
#include <string>
#include <string_view>

namespace klyrdb {

using txn_id_t = uint64_t;
inline constexpr txn_id_t INVALID_TXN_ID = 0;
inline constexpr lsn_t INVALID_LSN = 0;

inline constexpr uint32_t WAL_RECORD_MAGIC = 0x57414C52U; // ASCII "WALR"
inline constexpr size_t LOG_RECORD_HEADER_SIZE = 48;

enum class LogRecordType : uint16_t {
    Invalid    = 0x0000,
    Begin      = 0x0001,
    Commit     = 0x0002,
    Abort      = 0x0003,
    PageUpdate = 0x0004,
    PageAlloc  = 0x0005,
    Checkpoint = 0x0006
};

[[nodiscard]] constexpr std::string_view to_string(LogRecordType type) noexcept {
    switch (type) {
        case LogRecordType::Begin:      return "Begin";
        case LogRecordType::Commit:     return "Commit";
        case LogRecordType::Abort:      return "Abort";
        case LogRecordType::PageUpdate: return "PageUpdate";
        case LogRecordType::PageAlloc:  return "PageAlloc";
        case LogRecordType::Checkpoint: return "Checkpoint";
        case LogRecordType::Invalid:
        default:                        return "Invalid";
    }
}

template <typename OStream>
inline OStream& operator<<(OStream& os, LogRecordType type) {
    return os << to_string(type);
}

/// Represents a single Write-Ahead Log (WAL) record.
class LogRecord {
public:
    LogRecord() = default;

    // Header fields
    uint32_t magic{WAL_RECORD_MAGIC};
    LogRecordType type{LogRecordType::Invalid};
    uint16_t flags{0};
    lsn_t lsn{INVALID_LSN};
    lsn_t prev_lsn{INVALID_LSN};
    txn_id_t txn_id{INVALID_TXN_ID};
    page_id_t page_id{INVALID_PAGE_ID};
    uint32_t before_image_len{0};
    uint32_t after_image_len{0};
    uint32_t checksum{0};

    // Variable-length payloads
    std::vector<uint8_t> before_image;
    std::vector<uint8_t> after_image;

    [[nodiscard]] size_t total_size() const noexcept {
        return LOG_RECORD_HEADER_SIZE + before_image.size() + after_image.size();
    }

    /// Serializes the record into explicit big-endian bytes with computed CRC-32.
    void encode(std::vector<uint8_t>& dest) const;

    /// Decodes a log record from a raw byte buffer.
    [[nodiscard]] static Result<LogRecord> decode(std::span<const uint8_t> src);

    // Factory methods
    static LogRecord make_begin(lsn_t lsn, txn_id_t txn_id, lsn_t prev_lsn = INVALID_LSN);
    static LogRecord make_commit(lsn_t lsn, txn_id_t txn_id, lsn_t prev_lsn = INVALID_LSN);
    static LogRecord make_abort(lsn_t lsn, txn_id_t txn_id, lsn_t prev_lsn = INVALID_LSN);
    static LogRecord make_page_update(lsn_t lsn, txn_id_t txn_id, lsn_t prev_lsn, page_id_t page_id,
                                      std::span<const uint8_t> before, std::span<const uint8_t> after);
    static LogRecord make_page_alloc(lsn_t lsn, txn_id_t txn_id, lsn_t prev_lsn, page_id_t page_id,
                                     std::span<const uint8_t> before, std::span<const uint8_t> after);
    static LogRecord make_checkpoint(lsn_t lsn, lsn_t checkpoint_lsn, lsn_t min_required_lsn = 0,
                                     const std::vector<txn_id_t>& active_txns = {});
};

} // namespace klyrdb
