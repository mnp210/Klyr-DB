#include "klyrdb/sql/row.hpp"
#include "klyrdb/common/endian.hpp"
#include <cstring>

namespace klyrdb {

std::vector<uint8_t> SqlRow::encode(table_id_t table_id) const {
    // Calculate total required size
    // Header: version (1) + table_id (4) + column_count (2) = 7 bytes
    size_t total_size = 7;
    for (const auto& val : values_) {
        total_size += 1; // tag
        if (val.is_null()) {
            // tag only
        } else if (val.is_integer()) {
            total_size += 8;
        } else {
            total_size += 4 + val.as_text().size();
        }
    }

    std::vector<uint8_t> buffer(total_size);
    uint8_t* ptr = buffer.data();

    *ptr++ = ROW_FORMAT_VERSION;
    encode_u32_be(table_id, ptr);
    ptr += 4;
    encode_u16_be(static_cast<uint16_t>(values_.size()), ptr);
    ptr += 2;

    for (const auto& val : values_) {
        if (val.is_null()) {
            *ptr++ = static_cast<uint8_t>(SqlType::Null);
        } else if (val.is_integer()) {
            *ptr++ = static_cast<uint8_t>(SqlType::Integer);
            encode_i64_be(val.as_integer(), ptr);
            ptr += 8;
        } else {
            *ptr++ = static_cast<uint8_t>(SqlType::Text);
            const auto& text = val.as_text();
            encode_u32_be(static_cast<uint32_t>(text.size()), ptr);
            ptr += 4;
            if (!text.empty()) {
                std::memcpy(ptr, text.data(), text.size());
                ptr += text.size();
            }
        }
    }

    return buffer;
}

Result<table_id_t> SqlRow::peek_table_id(std::span<const uint8_t> bytes) noexcept {
    if (bytes.size() < 7) {
        return Status::page_corrupted("Record too small for row header");
    }
    if (bytes[0] != ROW_FORMAT_VERSION) {
        return Status::page_corrupted("Unsupported row format version: " + std::to_string(bytes[0]));
    }
    return decode_u32_be(bytes.data() + 1);
}

Result<SqlRow> SqlRow::decode(std::span<const uint8_t> bytes,
                              table_id_t expected_table_id,
                              const std::vector<SqlType>& expected_types) {
    if (bytes.size() < 7) {
        return Status::page_corrupted("Record too small for row header (size " +
                                      std::to_string(bytes.size()) + ")");
    }

    const uint8_t* ptr = bytes.data();
    const uint8_t* end = bytes.data() + bytes.size();

    const uint8_t version = *ptr++;
    if (version != ROW_FORMAT_VERSION) {
        return Status::page_corrupted("Unsupported row format version: " + std::to_string(version));
    }

    const table_id_t table_id = decode_u32_be(ptr);
    ptr += 4;

    if (table_id != expected_table_id) {
        return Status::page_corrupted("Row table_id " + std::to_string(table_id) +
                                      " does not match expected table_id " +
                                      std::to_string(expected_table_id));
    }

    const uint16_t col_count = decode_u16_be(ptr);
    ptr += 2;

    if (col_count != expected_types.size()) {
        return Status::page_corrupted("Row column count " + std::to_string(col_count) +
                                      " does not match schema column count " +
                                      std::to_string(expected_types.size()));
    }

    std::vector<SqlValue> values;
    values.reserve(col_count);

    for (size_t i = 0; i < col_count; ++i) {
        if (ptr >= end) {
            return Status::page_corrupted("Unexpected end of record reading column " + std::to_string(i));
        }

        const auto tag = static_cast<SqlType>(*ptr++);
        const SqlType expected_type = expected_types[i];

        if (tag == SqlType::Null) {
            values.push_back(SqlValue::null(expected_type));
        } else if (tag != expected_type) {
            return Status::type_mismatch("Column " + std::to_string(i) + " tag " +
                                         to_string(tag) + " does not match schema type " +
                                         to_string(expected_type));
        } else if (tag == SqlType::Integer) {
            if (static_cast<size_t>(end - ptr) < 8) {
                return Status::page_corrupted("Unexpected end of record reading INTEGER column " +
                                              std::to_string(i));
            }
            int64_t val = decode_i64_be(ptr);
            ptr += 8;
            values.emplace_back(val);
        } else if (tag == SqlType::Text) {
            if (static_cast<size_t>(end - ptr) < 4) {
                return Status::page_corrupted("Unexpected end of record reading TEXT length for column " +
                                              std::to_string(i));
            }
            uint32_t len = decode_u32_be(ptr);
            ptr += 4;
            if (static_cast<size_t>(end - ptr) < len) {
                return Status::page_corrupted("Unexpected end of record reading TEXT data for column " +
                                              std::to_string(i));
            }
            std::string text(reinterpret_cast<const char*>(ptr), len);
            ptr += len;
            values.emplace_back(std::move(text));
        } else {
            return Status::page_corrupted("Unknown SQL type tag: " + std::to_string(static_cast<uint8_t>(tag)));
        }
    }

    return SqlRow(std::move(values));
}

} // namespace klyrdb
