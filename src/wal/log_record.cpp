#include "klyrdb/wal/log_record.hpp"
#include "klyrdb/common/endian.hpp"
#include "klyrdb/common/checksum.hpp"
#include <cstring>

namespace klyrdb {

namespace {

constexpr size_t OFFSET_REC_MAGIC        = 0;  // 4B: uint32_t
constexpr size_t OFFSET_REC_TYPE         = 4;  // 2B: uint16_t
constexpr size_t OFFSET_REC_FLAGS        = 6;  // 2B: uint16_t
constexpr size_t OFFSET_REC_LSN          = 8;  // 8B: uint64_t
constexpr size_t OFFSET_REC_PREV_LSN     = 16; // 8B: uint64_t
constexpr size_t OFFSET_REC_TXN_ID       = 24; // 8B: uint64_t
constexpr size_t OFFSET_REC_PAGE_ID      = 32; // 4B: uint32_t
constexpr size_t OFFSET_REC_BEFORE_LEN   = 36; // 4B: uint32_t
constexpr size_t OFFSET_REC_AFTER_LEN    = 40; // 4B: uint32_t
constexpr size_t OFFSET_REC_CHECKSUM     = 44; // 4B: uint32_t
constexpr size_t OFFSET_REC_PAYLOAD      = 48;

} // namespace

void LogRecord::encode(std::vector<uint8_t>& dest) const {
    const size_t b_len = before_image.size();
    const size_t a_len = after_image.size();
    const size_t total = LOG_RECORD_HEADER_SIZE + b_len + a_len;

    dest.resize(total);
    uint8_t* ptr = dest.data();

    encode_u32_be(magic, ptr + OFFSET_REC_MAGIC);
    encode_u16_be(static_cast<uint16_t>(type), ptr + OFFSET_REC_TYPE);
    encode_u16_be(flags, ptr + OFFSET_REC_FLAGS);
    encode_u64_be(lsn, ptr + OFFSET_REC_LSN);
    encode_u64_be(prev_lsn, ptr + OFFSET_REC_PREV_LSN);
    encode_u64_be(txn_id, ptr + OFFSET_REC_TXN_ID);
    encode_u32_be(page_id, ptr + OFFSET_REC_PAGE_ID);
    encode_u32_be(static_cast<uint32_t>(b_len), ptr + OFFSET_REC_BEFORE_LEN);
    encode_u32_be(static_cast<uint32_t>(a_len), ptr + OFFSET_REC_AFTER_LEN);
    encode_u32_be(0, ptr + OFFSET_REC_CHECKSUM); // zero checksum during calculation

    if (b_len > 0) {
        std::memcpy(ptr + OFFSET_REC_PAYLOAD, before_image.data(), b_len);
    }
    if (a_len > 0) {
        std::memcpy(ptr + OFFSET_REC_PAYLOAD + b_len, after_image.data(), a_len);
    }

    uint32_t crc = calculate_crc32(dest);
    encode_u32_be(crc, ptr + OFFSET_REC_CHECKSUM);
}

Result<LogRecord> LogRecord::decode(std::span<const uint8_t> src) {
    if (src.size() < LOG_RECORD_HEADER_SIZE) {
        return Status::truncated_file("Buffer too small for LogRecord header");
    }

    const uint8_t* ptr = src.data();
    uint32_t rec_magic = decode_u32_be(ptr + OFFSET_REC_MAGIC);
    if (rec_magic != WAL_RECORD_MAGIC) {
        return Status::invalid_magic("Invalid WAL record magic signature: " + std::to_string(rec_magic));
    }

    uint16_t rec_type_raw = decode_u16_be(ptr + OFFSET_REC_TYPE);
    auto rec_type = static_cast<LogRecordType>(rec_type_raw);
    uint16_t rec_flags = decode_u16_be(ptr + OFFSET_REC_FLAGS);
    lsn_t rec_lsn = decode_u64_be(ptr + OFFSET_REC_LSN);
    lsn_t rec_prev_lsn = decode_u64_be(ptr + OFFSET_REC_PREV_LSN);
    txn_id_t rec_txn_id = decode_u64_be(ptr + OFFSET_REC_TXN_ID);
    page_id_t rec_page_id = decode_u32_be(ptr + OFFSET_REC_PAGE_ID);
    uint32_t b_len = decode_u32_be(ptr + OFFSET_REC_BEFORE_LEN);
    uint32_t a_len = decode_u32_be(ptr + OFFSET_REC_AFTER_LEN);
    uint32_t recorded_crc = decode_u32_be(ptr + OFFSET_REC_CHECKSUM);

    // Checked arithmetic for bounds
    if (b_len > PAGE_SIZE || a_len > PAGE_SIZE) {
        return Status::wal_corrupted("Image lengths exceed maximum page size in log record");
    }

    const size_t total_expected = LOG_RECORD_HEADER_SIZE + static_cast<size_t>(b_len) + static_cast<size_t>(a_len);
    if (src.size() < total_expected) {
        return Status::truncated_file("Buffer truncated for log record payload");
    }

    // Verify CRC-32
    std::vector<uint8_t> check_buf(src.begin(), src.begin() + total_expected);
    encode_u32_be(0, check_buf.data() + OFFSET_REC_CHECKSUM);
    uint32_t computed_crc = calculate_crc32(check_buf);
    if (computed_crc != recorded_crc) {
        return Status::checksum_mismatch("WAL record checksum mismatch: expected " +
                                        std::to_string(recorded_crc) + ", computed " +
                                        std::to_string(computed_crc));
    }

    LogRecord rec;
    rec.magic = rec_magic;
    rec.type = rec_type;
    rec.flags = rec_flags;
    rec.lsn = rec_lsn;
    rec.prev_lsn = rec_prev_lsn;
    rec.txn_id = rec_txn_id;
    rec.page_id = rec_page_id;
    rec.before_image_len = b_len;
    rec.after_image_len = a_len;
    rec.checksum = recorded_crc;

    if (b_len > 0) {
        rec.before_image.assign(ptr + OFFSET_REC_PAYLOAD, ptr + OFFSET_REC_PAYLOAD + b_len);
    }
    if (a_len > 0) {
        rec.after_image.assign(ptr + OFFSET_REC_PAYLOAD + b_len,
                               ptr + OFFSET_REC_PAYLOAD + b_len + a_len);
    }

    return rec;
}

LogRecord LogRecord::make_begin(lsn_t lsn, txn_id_t txn_id, lsn_t prev_lsn) {
    LogRecord rec;
    rec.type = LogRecordType::Begin;
    rec.lsn = lsn;
    rec.txn_id = txn_id;
    rec.prev_lsn = prev_lsn;
    return rec;
}

LogRecord LogRecord::make_commit(lsn_t lsn, txn_id_t txn_id, lsn_t prev_lsn) {
    LogRecord rec;
    rec.type = LogRecordType::Commit;
    rec.lsn = lsn;
    rec.txn_id = txn_id;
    rec.prev_lsn = prev_lsn;
    return rec;
}

LogRecord LogRecord::make_abort(lsn_t lsn, txn_id_t txn_id, lsn_t prev_lsn) {
    LogRecord rec;
    rec.type = LogRecordType::Abort;
    rec.lsn = lsn;
    rec.txn_id = txn_id;
    rec.prev_lsn = prev_lsn;
    return rec;
}

LogRecord LogRecord::make_page_update(lsn_t lsn, txn_id_t txn_id, lsn_t prev_lsn, page_id_t page_id,
                                     std::span<const uint8_t> before, std::span<const uint8_t> after) {
    LogRecord rec;
    rec.type = LogRecordType::PageUpdate;
    rec.lsn = lsn;
    rec.txn_id = txn_id;
    rec.prev_lsn = prev_lsn;
    rec.page_id = page_id;
    rec.before_image.assign(before.begin(), before.end());
    rec.after_image.assign(after.begin(), after.end());
    rec.before_image_len = static_cast<uint32_t>(rec.before_image.size());
    rec.after_image_len = static_cast<uint32_t>(rec.after_image.size());
    return rec;
}

LogRecord LogRecord::make_page_alloc(lsn_t lsn, txn_id_t txn_id, lsn_t prev_lsn, page_id_t page_id,
                                    std::span<const uint8_t> before, std::span<const uint8_t> after) {
    LogRecord rec = make_page_update(lsn, txn_id, prev_lsn, page_id, before, after);
    rec.type = LogRecordType::PageAlloc;
    return rec;
}

LogRecord LogRecord::make_checkpoint(lsn_t lsn, lsn_t checkpoint_lsn, lsn_t min_required_lsn,
                                     const std::vector<txn_id_t>& active_txns) {
    LogRecord rec;
    rec.type = LogRecordType::Checkpoint;
    rec.lsn = lsn;
    rec.prev_lsn = checkpoint_lsn; // checkpoint_lsn stored in prev_lsn
    if (min_required_lsn != 0 || !active_txns.empty()) {
        // Encode min_required_lsn (8 bytes) + active_txn_count (4 bytes) + active_txns
        rec.before_image.resize(12 + active_txns.size() * sizeof(txn_id_t));
        encode_u64_be(min_required_lsn, rec.before_image.data());
        encode_u32_be(static_cast<uint32_t>(active_txns.size()), rec.before_image.data() + 8);
        for (size_t i = 0; i < active_txns.size(); ++i) {
            encode_u64_be(active_txns[i], rec.before_image.data() + 12 + i * sizeof(txn_id_t));
        }
        rec.before_image_len = static_cast<uint32_t>(rec.before_image.size());
    }
    return rec;
}

} // namespace klyrdb
