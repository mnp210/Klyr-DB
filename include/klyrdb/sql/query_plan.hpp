#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/sql/ast.hpp"
#include "klyrdb/sql/value.hpp"
#include <string>
#include <vector>
#include <optional>
#include <sstream>
#include <iomanip>

namespace klyrdb {

/// Logical and Physical access paths supported by the query planner.
enum class AccessPathType {
    TableScan,
    IndexSeek,
    IndexRangeScan
};

[[nodiscard]] inline std::string to_string(AccessPathType path) {
    switch (path) {
        case AccessPathType::TableScan:      return "Table Scan";
        case AccessPathType::IndexSeek:      return "Index Seek";
        case AccessPathType::IndexRangeScan: return "Index Range Scan";
    }
    return "Unknown";
}

inline std::ostream& operator<<(std::ostream& os, AccessPathType path) {
    return os << to_string(path);
}

/// Representation of the index access condition (equality or range).
struct IndexAccessPredicate {
    std::string column_name;
    std::string index_name;
    page_id_t root_page_id{INVALID_PAGE_ID};
    bool is_unique{false};
    bool is_range{false};

    // For Equality Seek:
    SqlValue exact_value;

    // For Range Scan:
    std::optional<SqlValue> low_value;
    bool inclusive_low{true};
    std::optional<SqlValue> high_value;
    bool inclusive_high{true};

    [[nodiscard]] std::string predicate_string() const {
        if (!is_range) {
            return column_name + " = " + exact_value.to_string();
        }
        std::string s;
        if (low_value.has_value()) {
            s += column_name + (inclusive_low ? " >= " : " > ") + low_value->to_string();
        }
        if (high_value.has_value()) {
            if (!s.empty()) s += " AND ";
            s += column_name + (inclusive_high ? " <= " : " < ") + high_value->to_string();
        }
        return s.empty() ? "(full range)" : s;
    }
};

/// Estimated work and cardinality of a query plan.
struct PlanCost {
    double estimated_cost{0.0};
    size_t estimated_rows{0};
    size_t estimated_pages{0};
};

/// Runtime instrumentation metrics collected during plan execution.
struct ExecutionMetrics {
    size_t rows_examined{0};
    size_t rows_produced{0};
    uint64_t elapsed_time_ns{0};
    uint64_t planning_time_us{0};
    uint64_t execution_time_us{0};
};

/// Complete executable and explainable query execution plan.
struct QueryPlan {
    std::string table_name;
    table_id_t table_id{INVALID_TABLE_ID};
    AccessPathType access_path{AccessPathType::TableScan};
    std::optional<IndexAccessPredicate> index_access;
    std::vector<BinaryPredicate> residual_filters;

    std::optional<OrderByClause> order_by;
    bool order_by_satisfied_by_index{false};
    bool scan_descending{false};

    std::optional<LimitClause> limit;
    bool limit_pushed_down{false};

    std::vector<std::string> projected_columns;
    bool select_all{false};

    PlanCost cost;
    mutable ExecutionMetrics metrics;

    [[nodiscard]] std::string explain_text() const;
    [[nodiscard]] std::string explain_analyze_text() const;
};

} // namespace klyrdb
