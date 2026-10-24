#pragma once

#include "klyrdb/sql/value.hpp"
#include "klyrdb/sql/row.hpp"
#include <string>
#include <vector>

namespace klyrdb {

enum class QueryResultType {
    Rows,
    Count,
    Message
};

class QueryResult {
public:
    QueryResult() : type_(QueryResultType::Message), message_("OK") {}

    static QueryResult make_rows(std::vector<std::string> col_names,
                                 std::vector<SqlType> col_types,
                                 std::vector<SqlRow> rows) {
        QueryResult r;
        r.type_ = QueryResultType::Rows;
        r.column_names_ = std::move(col_names);
        r.column_types_ = std::move(col_types);
        r.rows_ = std::move(rows);
        return r;
    }

    static QueryResult make_count(size_t count, std::string action = "Rows affected") {
        QueryResult r;
        r.type_ = QueryResultType::Count;
        r.rows_affected_ = count;
        r.message_ = action + ": " + std::to_string(count);
        return r;
    }

    static QueryResult make_message(std::string msg) {
        QueryResult r;
        r.type_ = QueryResultType::Message;
        r.message_ = std::move(msg);
        return r;
    }

    [[nodiscard]] QueryResultType type() const noexcept { return type_; }
    [[nodiscard]] bool is_rows() const noexcept { return type_ == QueryResultType::Rows; }
    [[nodiscard]] bool is_count() const noexcept { return type_ == QueryResultType::Count; }
    [[nodiscard]] bool is_message() const noexcept { return type_ == QueryResultType::Message; }

    [[nodiscard]] size_t rows_affected() const noexcept { return rows_affected_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] const std::vector<std::string>& column_names() const noexcept { return column_names_; }
    [[nodiscard]] const std::vector<SqlType>& column_types() const noexcept { return column_types_; }
    [[nodiscard]] const std::vector<SqlRow>& rows() const noexcept { return rows_; }
    [[nodiscard]] size_t row_count() const noexcept { return rows_.size(); }

private:
    QueryResultType type_{QueryResultType::Message};
    size_t rows_affected_{0};
    std::string message_;
    std::vector<std::string> column_names_;
    std::vector<SqlType> column_types_;
    std::vector<SqlRow> rows_;
};

} // namespace klyrdb
