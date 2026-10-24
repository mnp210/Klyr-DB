#pragma once

#include "klyrdb/common/result.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/sql/value.hpp"
#include "klyrdb/sql/query_result.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <optional>

namespace klyrdb {

/// Represents a strongly-typed or dynamically-typed SQL value.
using Value = SqlValue;

/// Represents an immutable single row in a query result set.
class Row {
public:
    Row() = default;
    Row(std::vector<Value> values, std::shared_ptr<const std::vector<std::string>> col_names);

    /// Total number of columns in this row.
    [[nodiscard]] size_t size() const noexcept { return values_.size(); }

    /// Returns true if the row contains zero columns.
    [[nodiscard]] bool empty() const noexcept { return values_.empty(); }

    /// Access column value by zero-based index. Throws std::out_of_range if index is invalid.
    [[nodiscard]] const Value& get(size_t index) const;

    /// Access column value by zero-based index. Throws std::out_of_range if index is invalid.
    [[nodiscard]] const Value& operator[](size_t index) const { return get(index); }

    /// Access column value by column name (case-insensitive). Throws std::out_of_range if column is not found.
    [[nodiscard]] const Value& get(std::string_view col_name) const;

    /// Access column value by column name (case-insensitive). Throws std::out_of_range if column is not found.
    [[nodiscard]] const Value& operator[](std::string_view col_name) const { return get(col_name); }

    /// Returns underlying column values.
    [[nodiscard]] const std::vector<Value>& values() const noexcept { return values_; }

private:
    std::vector<Value> values_;
    std::shared_ptr<const std::vector<std::string>> col_names_;
};

/// Represents the tabular result or mutation summary returned from executing a SQL statement.
class ResultSet {
public:
    ResultSet() = default;
    explicit ResultSet(QueryResult internal_result);

    /// Returns true if this result contains rows (e.g. from a SELECT statement).
    [[nodiscard]] bool is_rows() const noexcept { return type_ == QueryResultType::Rows; }

    /// Returns true if this result represents a mutation count (e.g. INSERT, UPDATE, DELETE).
    [[nodiscard]] bool is_count() const noexcept { return type_ == QueryResultType::Count; }

    /// Returns true if this result contains an informative message (e.g. DDL statements).
    [[nodiscard]] bool is_message() const noexcept { return type_ == QueryResultType::Message; }

    /// Returns the number of rows returned.
    [[nodiscard]] size_t row_count() const noexcept { return rows_.size(); }

    /// Returns the number of columns in each row.
    [[nodiscard]] size_t column_count() const noexcept;

    /// Returns the number of rows affected by an INSERT, UPDATE, or DELETE statement.
    [[nodiscard]] size_t rows_affected() const noexcept { return rows_affected_; }

    /// Returns the summary message or action description.
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    /// Returns the column names in order.
    [[nodiscard]] const std::vector<std::string>& column_names() const noexcept;

    /// Returns all rows in the result set.
    [[nodiscard]] const std::vector<Row>& rows() const noexcept { return rows_; }

    // Range-based for loop iterators
    [[nodiscard]] auto begin() const noexcept { return rows_.begin(); }
    [[nodiscard]] auto end() const noexcept { return rows_.end(); }

    /// Direct row access by index. Throws std::out_of_range if index is invalid.
    [[nodiscard]] const Row& operator[](size_t index) const { return rows_.at(index); }

private:
    QueryResultType type_{QueryResultType::Message};
    size_t rows_affected_{0};
    std::string message_;
    std::shared_ptr<const std::vector<std::string>> column_names_;
    std::vector<Row> rows_;
};

} // namespace klyrdb
