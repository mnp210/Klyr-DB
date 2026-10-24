#include "klyrdb/result.hpp"
#include <algorithm>
#include <stdexcept>

namespace klyrdb {

static const std::vector<std::string> g_empty_columns;

Row::Row(std::vector<Value> values, std::shared_ptr<const std::vector<std::string>> col_names)
    : values_(std::move(values)), col_names_(std::move(col_names)) {}

const Value& Row::get(size_t index) const {
    if (index >= values_.size()) {
        throw std::out_of_range("Column index " + std::to_string(index) +
                                " out of range (row size: " + std::to_string(values_.size()) + ")");
    }
    return values_[index];
}

static bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

const Value& Row::get(std::string_view col_name) const {
    if (!col_names_) {
        throw std::out_of_range("Column names not available for this row");
    }
    for (size_t i = 0; i < col_names_->size(); ++i) {
        if (iequals((*col_names_)[i], col_name)) {
            return get(i);
        }
    }
    throw std::out_of_range("Column '" + std::string(col_name) + "' not found in row");
}

ResultSet::ResultSet(QueryResult internal_result)
    : type_(internal_result.type()),
      rows_affected_(internal_result.rows_affected()),
      message_(internal_result.message()) {
    if (internal_result.is_rows()) {
        column_names_ = std::make_shared<const std::vector<std::string>>(internal_result.column_names());
        rows_.reserve(internal_result.rows().size());
        for (const auto& sql_row : internal_result.rows()) {
            rows_.emplace_back(sql_row.values(), column_names_);
        }
    }
}

size_t ResultSet::column_count() const noexcept {
    return column_names_ ? column_names_->size() : 0;
}

const std::vector<std::string>& ResultSet::column_names() const noexcept {
    return column_names_ ? *column_names_ : g_empty_columns;
}

} // namespace klyrdb
