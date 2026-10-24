#include "klyrdb/storage/record.hpp"
#include "klyrdb/common/endian.hpp"
#include <cstring>

namespace klyrdb {

size_t Record::serialized_size() const noexcept {
    // 1 byte version + 2 bytes field count
    size_t size = 1 + 2;
    for (const auto& field : fields_) {
        // 1 byte type tag
        size += 1;
        std::visit([&size](const auto& val) {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                // No payload
            } else if constexpr (std::is_same_v<T, int32_t>) {
                size += 4;
            } else if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, double>) {
                size += 8;
            } else if constexpr (std::is_same_v<T, std::string>) {
                size += 4 + val.size(); // 4-byte length prefix + string bytes
            } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
                size += 4 + val.size(); // 4-byte length prefix + blob bytes
            }
        }, field);
    }
    return size;
}

std::vector<uint8_t> Record::serialize() const {
    std::vector<uint8_t> buffer(serialized_size());
    auto res = serialize(std::span<uint8_t>(buffer));
    if (res.is_err()) {
        throw std::runtime_error("Record serialization failed: " + res.status().to_string());
    }
    return buffer;
}

Result<size_t> Record::serialize(std::span<uint8_t> dest) const noexcept {
    const size_t required_size = serialized_size();
    if (dest.size() < required_size) {
        return Status::payload_overflow("Destination buffer too small: need " +
                                        std::to_string(required_size) + ", have " +
                                        std::to_string(dest.size()));
    }

    if (fields_.size() > 65535) {
        return Status::invalid_argument("Record field count exceeds maximum 65535");
    }

    uint8_t* ptr = dest.data();
    *ptr++ = RECORD_FORMAT_VERSION;

    encode_u16_be(static_cast<uint16_t>(fields_.size()), ptr);
    ptr += 2;

    for (const auto& field : fields_) {
        std::visit([&ptr](const auto& val) {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                *ptr++ = static_cast<uint8_t>(FieldType::Null);
            } else if constexpr (std::is_same_v<T, int32_t>) {
                *ptr++ = static_cast<uint8_t>(FieldType::Int32);
                encode_i32_be(val, ptr);
                ptr += 4;
            } else if constexpr (std::is_same_v<T, int64_t>) {
                *ptr++ = static_cast<uint8_t>(FieldType::Int64);
                encode_i64_be(val, ptr);
                ptr += 8;
            } else if constexpr (std::is_same_v<T, double>) {
                *ptr++ = static_cast<uint8_t>(FieldType::Float64);
                encode_f64_be(val, ptr);
                ptr += 8;
            } else if constexpr (std::is_same_v<T, std::string>) {
                *ptr++ = static_cast<uint8_t>(FieldType::String);
                encode_u32_be(static_cast<uint32_t>(val.size()), ptr);
                ptr += 4;
                if (!val.empty()) {
                    std::memcpy(ptr, val.data(), val.size());
                    ptr += val.size();
                }
            } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
                *ptr++ = static_cast<uint8_t>(FieldType::Bytes);
                encode_u32_be(static_cast<uint32_t>(val.size()), ptr);
                ptr += 4;
                if (!val.empty()) {
                    std::memcpy(ptr, val.data(), val.size());
                    ptr += val.size();
                }
            }
        }, field);
    }

    return required_size;
}

Result<Record> Record::deserialize(std::span<const uint8_t> src) noexcept {
    if (src.size() < 3) {
        return Status::truncated_file("Buffer too small for record header: " +
                                      std::to_string(src.size()) + " < 3");
    }

    const uint8_t* ptr = src.data();
    const uint8_t* end = src.data() + src.size();

    const uint8_t version = *ptr++;
    if (version != RECORD_FORMAT_VERSION) {
        return Status::unsupported_version("Unsupported record version: " +
                                           std::to_string(version));
    }

    const uint16_t field_count = decode_u16_be(ptr);
    ptr += 2;

    std::vector<FieldValue> fields;
    fields.reserve(field_count);

    for (uint16_t i = 0; i < field_count; ++i) {
        if (ptr >= end) {
            return Status::truncated_file("Truncated record: unexpected end before field type");
        }

        const auto type = static_cast<FieldType>(*ptr++);
        switch (type) {
            case FieldType::Null: {
                fields.emplace_back(std::monostate{});
                break;
            }
            case FieldType::Int32: {
                if (end - ptr < 4) {
                    return Status::truncated_file("Truncated Int32 field at index " + std::to_string(i));
                }
                fields.emplace_back(decode_i32_be(ptr));
                ptr += 4;
                break;
            }
            case FieldType::Int64: {
                if (end - ptr < 8) {
                    return Status::truncated_file("Truncated Int64 field at index " + std::to_string(i));
                }
                fields.emplace_back(decode_i64_be(ptr));
                ptr += 8;
                break;
            }
            case FieldType::Float64: {
                if (end - ptr < 8) {
                    return Status::truncated_file("Truncated Float64 field at index " + std::to_string(i));
                }
                fields.emplace_back(decode_f64_be(ptr));
                ptr += 8;
                break;
            }
            case FieldType::String: {
                if (end - ptr < 4) {
                    return Status::truncated_file("Truncated String length at index " + std::to_string(i));
                }
                const uint32_t len = decode_u32_be(ptr);
                ptr += 4;
                if (static_cast<size_t>(end - ptr) < len) {
                    return Status::truncated_file("Truncated String payload at index " + std::to_string(i));
                }
                std::string s(reinterpret_cast<const char*>(ptr), len);
                ptr += len;
                fields.emplace_back(std::move(s));
                break;
            }
            case FieldType::Bytes: {
                if (end - ptr < 4) {
                    return Status::truncated_file("Truncated Bytes length at index " + std::to_string(i));
                }
                const uint32_t len = decode_u32_be(ptr);
                ptr += 4;
                if (static_cast<size_t>(end - ptr) < len) {
                    return Status::truncated_file("Truncated Bytes payload at index " + std::to_string(i));
                }
                std::vector<uint8_t> bytes(ptr, ptr + len);
                ptr += len;
                fields.emplace_back(std::move(bytes));
                break;
            }
            default:
                return Status::page_corrupted("Unknown field type tag: " +
                                              std::to_string(static_cast<uint8_t>(type)));
        }
    }

    return Record(std::move(fields));
}

} // namespace klyrdb
