#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/sql/value.hpp"
#include <vector>
#include <span>
#include <cstdint>

namespace klyrdb {

using table_id_t = uint32_t;
inline constexpr table_id_t INVALID_TABLE_ID = 0;
inline constexpr uint8_t ROW_FORMAT_VERSION = 1;

/// Logical representation of a typed row of values belonging to a table.
class SqlRow {
public:
    SqlRow() = default;
    explicit SqlRow(std::vector<SqlValue> values) : values_(std::move(values)) {}

    [[nodiscard]] size_t column_count() const noexcept { return values_.size(); }
    [[nodiscard]] const std::vector<SqlValue>& values() const noexcept { return values_; }
    [[nodiscard]] std::vector<SqlValue>& values() noexcept { return values_; }

    [[nodiscard]] const SqlValue& get(size_t index) const { return values_.at(index); }
    void set(size_t index, SqlValue val) { values_.at(index) = std::move(val); }
    void append(SqlValue val) { values_.push_back(std::move(val)); }

    /// Encodes the row into a portable binary format prefixed by table_id.
    [[nodiscard]] std::vector<uint8_t> encode(table_id_t table_id) const;

    /// Decodes a row from bytes, strictly validating the format, table_id, and expected column types.
    [[nodiscard]] static Result<SqlRow> decode(std::span<const uint8_t> bytes,
                                               table_id_t expected_table_id,
                                               const std::vector<SqlType>& expected_types);

    /// Fast inspection of the table_id without decoding the entire row payload.
    [[nodiscard]] static Result<table_id_t> peek_table_id(std::span<const uint8_t> bytes) noexcept;

private:
    std::vector<SqlValue> values_;
};

} // namespace klyrdb
