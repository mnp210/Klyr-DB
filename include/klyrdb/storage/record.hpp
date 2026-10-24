#pragma once

#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <span>

namespace klyrdb {

/// Field data types supported by KlyrDB record serialization.
enum class FieldType : uint8_t {
    Null    = 0x00,
    Int32   = 0x01,
    Int64   = 0x02,
    Float64 = 0x03,
    String  = 0x04,
    Bytes   = 0x05
};

/// Type-safe variant representing a single column field value.
using FieldValue = std::variant<
    std::monostate,          // Null
    int32_t,                 // Int32
    int64_t,                 // Int64
    double,                  // Float64
    std::string,             // UTF-8 String
    std::vector<uint8_t>     // Binary Blob / Bytes
>;

inline constexpr uint8_t RECORD_FORMAT_VERSION = 1;

/// Represents a versioned, length-checked tuple of fields.
class Record {
public:
    Record() noexcept = default;
    explicit Record(std::vector<FieldValue> fields) noexcept : fields_(std::move(fields)) {}

    void add_null() { fields_.emplace_back(std::monostate{}); }
    void add_int32(int32_t val) { fields_.emplace_back(val); }
    void add_int64(int64_t val) { fields_.emplace_back(val); }
    void add_float64(double val) { fields_.emplace_back(val); }
    void add_string(std::string str) { fields_.emplace_back(std::move(str)); }
    void add_bytes(std::vector<uint8_t> bytes) { fields_.emplace_back(std::move(bytes)); }

    [[nodiscard]] size_t field_count() const noexcept { return fields_.size(); }
    [[nodiscard]] const std::vector<FieldValue>& fields() const noexcept { return fields_; }
    [[nodiscard]] const FieldValue& field(size_t index) const { return fields_.at(index); }

    /// Calculates the exact serialized byte length of this record.
    [[nodiscard]] size_t serialized_size() const noexcept;

    /// Serializes the record into an allocated byte vector.
    [[nodiscard]] std::vector<uint8_t> serialize() const;

    /// Serializes the record into a provided destination span, returning bytes written.
    [[nodiscard]] Result<size_t> serialize(std::span<uint8_t> dest) const noexcept;

    /// Deserializes a record from a binary span with strict length checks.
    [[nodiscard]] static Result<Record> deserialize(std::span<const uint8_t> src) noexcept;

    bool operator==(const Record& other) const = default;

private:
    std::vector<FieldValue> fields_;
};

} // namespace klyrdb
