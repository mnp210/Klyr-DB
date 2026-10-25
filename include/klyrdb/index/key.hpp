#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/common/endian.hpp"
#include "klyrdb/storage/record_id.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <compare>
#include <span>

namespace klyrdb {

/// Key data type enumeration for B+ Tree indexes.
enum class KeyType : uint8_t {
    Int64  = 0x01,
    String = 0x02
};

inline constexpr size_t MAX_KEY_STRING_LENGTH = 128;

/// Strongly-typed key representation supporting signed 64-bit integers and UTF-8 strings.
class IndexKey {
public:
    IndexKey() noexcept : type_(KeyType::Int64), value_(int64_t{0}) {}
    explicit IndexKey(int64_t val) noexcept : type_(KeyType::Int64), value_(val) {}
    explicit IndexKey(std::string str) : type_(KeyType::String), value_(std::move(str)) {
        if (std::get<std::string>(value_).size() > MAX_KEY_STRING_LENGTH) {
            throw std::invalid_argument("Key string length exceeds maximum of " +
                                       std::to_string(MAX_KEY_STRING_LENGTH));
        }
    }
    explicit IndexKey(std::string_view str) : IndexKey(std::string(str)) {}
    explicit IndexKey(const char* str) : IndexKey(std::string(str)) {}

    [[nodiscard]] KeyType type() const noexcept { return type_; }
    [[nodiscard]] bool is_int64() const noexcept { return type_ == KeyType::Int64; }
    [[nodiscard]] bool is_string() const noexcept { return type_ == KeyType::String; }

    [[nodiscard]] int64_t as_int64() const { return std::get<int64_t>(value_); }
    [[nodiscard]] const std::string& as_string() const { return std::get<std::string>(value_); }

    // Strict Total Ordering
    bool operator==(const IndexKey& other) const noexcept {
        if (type_ != other.type_) return false;
        return value_ == other.value_;
    }

    bool operator!=(const IndexKey& other) const noexcept {
        return !(*this == other);
    }

    bool operator<(const IndexKey& other) const noexcept {
        if (type_ != other.type_) {
            return static_cast<uint8_t>(type_) < static_cast<uint8_t>(other.type_);
        }
        if (type_ == KeyType::Int64) {
            return std::get<int64_t>(value_) < std::get<int64_t>(other.value_);
        }
        return std::get<std::string>(value_) < std::get<std::string>(other.value_);
    }

    bool operator<=(const IndexKey& other) const noexcept {
        return !(other < *this);
    }

    bool operator>(const IndexKey& other) const noexcept {
        return other < *this;
    }

    bool operator>=(const IndexKey& other) const noexcept {
        return !(*this < other);
    }

    [[nodiscard]] size_t serialized_size() const noexcept {
        if (type_ == KeyType::Int64) {
            return 1 + 8; // type + int64
        }
        return 1 + 2 + std::get<std::string>(value_).size(); // type + uint16 len + bytes
    }

    void encode(uint8_t* out) const noexcept {
        *out++ = static_cast<uint8_t>(type_);
        if (type_ == KeyType::Int64) {
            encode_i64_be(std::get<int64_t>(value_), out);
        } else {
            const auto& str = std::get<std::string>(value_);
            encode_u16_be(static_cast<uint16_t>(str.size()), out);
            out += 2;
            if (!str.empty()) {
                std::memcpy(out, str.data(), str.size());
            }
        }
    }

    [[nodiscard]] static Result<IndexKey> decode(const uint8_t* in, size_t max_len) noexcept {
        if (max_len < 1) {
            return Status::truncated_file("Buffer too small for key type");
        }
        const auto type = static_cast<KeyType>(*in++);
        if (type == KeyType::Int64) {
            if (max_len < 9) {
                return Status::truncated_file("Buffer too small for Int64 key");
            }
            return IndexKey(decode_i64_be(in));
        } else if (type == KeyType::String) {
            if (max_len < 3) {
                return Status::truncated_file("Buffer too small for string key length");
            }
            const uint16_t len = decode_u16_be(in);
            in += 2;
            if (len > MAX_KEY_STRING_LENGTH) {
                return Status::key_too_large("Key string length " + std::to_string(len) +
                                            " exceeds max " + std::to_string(MAX_KEY_STRING_LENGTH));
            }
            if (max_len < 3 + static_cast<size_t>(len)) {
                return Status::truncated_file("Buffer too small for string key data");
            }
            return IndexKey(std::string(reinterpret_cast<const char*>(in), len));
        }
        return Status::index_corrupted("Unknown KeyType tag: " + std::to_string(static_cast<uint8_t>(type)));
    }

    [[nodiscard]] std::string to_string() const {
        if (type_ == KeyType::Int64) {
            return std::to_string(std::get<int64_t>(value_));
        }
        return "\"" + std::get<std::string>(value_) + "\"";
    }

private:
    KeyType type_{KeyType::Int64};
    std::variant<int64_t, std::string> value_{int64_t{0}};
};

template <typename OStream>
inline OStream& operator<<(OStream& os, const IndexKey& key) {
    return os << key.to_string();
}

/// A complete B+ tree leaf entry pairing a search key with its target RecordId.
/// Sorted by (key, rid) to support duplicate logical keys with a strict total order.
struct IndexEntry {
    IndexKey key;
    RecordId rid;

    IndexEntry() noexcept = default;
    IndexEntry(IndexKey k, RecordId r) noexcept : key(std::move(k)), rid(r) {}

    bool operator==(const IndexEntry& other) const noexcept {
        return key == other.key && rid == other.rid;
    }

    bool operator!=(const IndexEntry& other) const noexcept {
        return !(*this == other);
    }

    bool operator<(const IndexEntry& other) const noexcept {
        if (key != other.key) {
            return key < other.key;
        }
        return rid < other.rid;
    }

    bool operator<=(const IndexEntry& other) const noexcept {
        return !(other < *this);
    }

    bool operator>(const IndexEntry& other) const noexcept {
        return other < *this;
    }

    bool operator>=(const IndexEntry& other) const noexcept {
        return !(*this < other);
    }

    [[nodiscard]] std::string to_string() const {
        return "(" + key.to_string() + " -> " + rid.to_string() + ")";
    }
};

template <typename OStream>
inline OStream& operator<<(OStream& os, const IndexEntry& entry) {
    return os << entry.to_string();
}

} // namespace klyrdb
