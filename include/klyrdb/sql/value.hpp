#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/index/key.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <compare>
#include <stdexcept>
#include <iostream>

namespace klyrdb {

/// Supported SQL data types for KlyrDB.
enum class SqlType : uint8_t {
    Null    = 0x00,
    Integer = 0x01,
    Text    = 0x02
};

[[nodiscard]] inline std::string to_string(SqlType type) {
    switch (type) {
        case SqlType::Null:    return "NULL";
        case SqlType::Integer: return "INTEGER";
        case SqlType::Text:    return "TEXT";
        default:               return "UNKNOWN";
    }
}

template <typename OStream>
inline OStream& operator<<(OStream& os, SqlType type) {
    os << to_string(type);
    return os;
}

[[nodiscard]] inline Result<SqlType> parse_sql_type(std::string_view str) {
    std::string upper;
    upper.reserve(str.size());
    for (char c : str) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    if (upper == "INTEGER" || upper == "INT" || upper == "BIGINT") {
        return SqlType::Integer;
    }
    if (upper == "TEXT" || upper == "VARCHAR" || upper == "STRING") {
        return SqlType::Text;
    }
    return Status::unsupported_sql("Unsupported data type: " + std::string(str));
}

/// Dynamically typed SQL value supporting NULL, INTEGER (int64_t) and TEXT (std::string).
class SqlValue {
public:
    SqlValue() noexcept : type_(SqlType::Null), value_(std::monostate{}) {}
    explicit SqlValue(std::nullptr_t) noexcept : type_(SqlType::Null), value_(std::monostate{}) {}
    explicit SqlValue(int64_t val) noexcept : type_(SqlType::Integer), value_(val) {}
    explicit SqlValue(int32_t val) noexcept : type_(SqlType::Integer), value_(static_cast<int64_t>(val)) {}
    explicit SqlValue(std::string val) noexcept : type_(SqlType::Text), value_(std::move(val)) {}
    explicit SqlValue(std::string_view val) : type_(SqlType::Text), value_(std::string(val)) {}
    explicit SqlValue(const char* val) : type_(SqlType::Text), value_(std::string(val)) {}

    [[nodiscard]] static SqlValue null(SqlType type_hint = SqlType::Null) noexcept {
        SqlValue v;
        v.type_ = type_hint == SqlType::Null ? SqlType::Null : type_hint;
        v.value_ = std::monostate{};
        return v;
    }

    [[nodiscard]] SqlType type() const noexcept { return type_; }
    [[nodiscard]] bool is_null() const noexcept { return std::holds_alternative<std::monostate>(value_); }
    [[nodiscard]] bool is_integer() const noexcept { return type_ == SqlType::Integer && !is_null(); }
    [[nodiscard]] bool is_text() const noexcept { return type_ == SqlType::Text && !is_null(); }

    [[nodiscard]] int64_t as_integer() const {
        if (!is_integer()) {
            throw std::runtime_error("SqlValue is not an INTEGER (is_null=" + std::to_string(is_null()) + ")");
        }
        return std::get<int64_t>(value_);
    }

    [[nodiscard]] const std::string& as_text() const {
        if (!is_text()) {
            throw std::runtime_error("SqlValue is not a TEXT (is_null=" + std::to_string(is_null()) + ")");
        }
        return std::get<std::string>(value_);
    }

    [[nodiscard]] std::string to_string() const {
        if (is_null()) {
            return "NULL";
        }
        if (is_integer()) {
            return std::to_string(std::get<int64_t>(value_));
        }
        return std::get<std::string>(value_);
    }

    [[nodiscard]] IndexKey to_index_key() const {
        if (is_null()) {
            return IndexKey("");
        }
        if (is_integer()) {
            return IndexKey(std::get<int64_t>(value_));
        }
        return IndexKey(std::get<std::string>(value_));
    }

    [[nodiscard]] static SqlValue from_index_key(const IndexKey& key) {
        if (key.type() == KeyType::Int64) {
            return SqlValue(key.as_int64());
        }
        return SqlValue(key.as_string());
    }

    // Comparison operations
    [[nodiscard]] bool operator==(const SqlValue& other) const noexcept {
        if (is_null() && other.is_null()) return true;
        if (is_null() || other.is_null()) return false;
        if (type_ != other.type_) return false;
        return value_ == other.value_;
    }

    [[nodiscard]] bool operator!=(const SqlValue& other) const noexcept {
        return !(*this == other);
    }

    [[nodiscard]] bool operator<(const SqlValue& other) const noexcept {
        if (is_null() && !other.is_null()) return true;
        if (!is_null() && other.is_null()) return false;
        if (is_null() && other.is_null()) return false;
        if (type_ != other.type_) {
            return static_cast<uint8_t>(type_) < static_cast<uint8_t>(other.type_);
        }
        return value_ < other.value_;
    }

    [[nodiscard]] bool operator<=(const SqlValue& other) const noexcept {
        return !(other < *this);
    }

    [[nodiscard]] bool operator>(const SqlValue& other) const noexcept {
        return other < *this;
    }

    [[nodiscard]] bool operator>=(const SqlValue& other) const noexcept {
        return !(*this < other);
    }

private:
    SqlType type_{SqlType::Null};
    std::variant<std::monostate, int64_t, std::string> value_{std::monostate{}};
};

inline std::ostream& operator<<(std::ostream& os, const SqlValue& val) {
    os << val.to_string();
    return os;
}

} // namespace klyrdb
