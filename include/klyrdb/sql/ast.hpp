#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/sql/value.hpp"
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <limits>

namespace klyrdb {

enum class BinaryOp {
    Equal,
    NotEqual,
    LessThan,
    LessEqual,
    GreaterThan,
    GreaterEqual
};

[[nodiscard]] inline std::string to_string(BinaryOp op) {
    switch (op) {
        case BinaryOp::Equal:        return "=";
        case BinaryOp::NotEqual:     return "!=";
        case BinaryOp::LessThan:     return "<";
        case BinaryOp::LessEqual:    return "<=";
        case BinaryOp::GreaterThan:  return ">";
        case BinaryOp::GreaterEqual: return ">=";
    }
    return "?";
}

template <typename OStream>
inline OStream& operator<<(OStream& os, BinaryOp op) {
    os << to_string(op);
    return os;
}

struct BinaryPredicate {
    std::string column_name;
    BinaryOp op{BinaryOp::Equal};
    SqlValue value;

    BinaryPredicate() = default;
    BinaryPredicate(std::string col, BinaryOp operation, SqlValue val)
        : column_name(std::move(col)), op(operation), value(std::move(val)) {}

    [[nodiscard]] bool evaluate(const SqlValue& col_val) const {
        if (col_val.is_null() || value.is_null()) {
            if (op == BinaryOp::Equal) return col_val.is_null() && value.is_null();
            if (op == BinaryOp::NotEqual) return col_val.is_null() != value.is_null();
            return false;
        }
        if (col_val.type() != value.type()) {
            return false;
        }
        switch (op) {
            case BinaryOp::Equal:        return col_val == value;
            case BinaryOp::NotEqual:     return col_val != value;
            case BinaryOp::LessThan:     return col_val < value;
            case BinaryOp::LessEqual:    return col_val <= value;
            case BinaryOp::GreaterThan:  return col_val > value;
            case BinaryOp::GreaterEqual: return col_val >= value;
        }
        return false;
    }
};

struct WhereClause {
    std::vector<BinaryPredicate> predicates;

    [[nodiscard]] bool empty() const noexcept { return predicates.empty(); }
    [[nodiscard]] size_t size() const noexcept { return predicates.size(); }
};

struct OrderByClause {
    std::string column_name;
    bool ascending{true};
};

struct LimitClause {
    size_t limit{std::numeric_limits<size_t>::max()};
    bool has_limit{false};
};

enum class StatementType {
    CreateTable,
    CreateIndex,
    DropTable,
    DropIndex,
    Insert,
    Select,
    Update,
    Delete,
    Begin,
    Commit,
    Rollback,
    Explain,
    Analyze
};

template <typename OStream>
inline OStream& operator<<(OStream& os, StatementType type) {
    switch (type) {
        case StatementType::CreateTable: return os << "CreateTable";
        case StatementType::CreateIndex: return os << "CreateIndex";
        case StatementType::DropTable:   return os << "DropTable";
        case StatementType::DropIndex:   return os << "DropIndex";
        case StatementType::Insert:      return os << "Insert";
        case StatementType::Select:      return os << "Select";
        case StatementType::Update:      return os << "Update";
        case StatementType::Delete:      return os << "Delete";
        case StatementType::Begin:       return os << "Begin";
        case StatementType::Commit:      return os << "Commit";
        case StatementType::Rollback:    return os << "Rollback";
        case StatementType::Explain:     return os << "Explain";
        case StatementType::Analyze:     return os << "Analyze";
    }
    return os << "Unknown";
}

struct Statement {
    virtual ~Statement() = default;
    [[nodiscard]] virtual StatementType type() const = 0;
};

struct CreateTableStatement : Statement {
    std::string table_name;
    std::vector<ColumnDef> columns;
    std::vector<TableConstraint> constraints;
    [[nodiscard]] StatementType type() const override { return StatementType::CreateTable; }
};

struct CreateIndexStatement : Statement {
    std::string index_name;
    std::string table_name;
    std::string column_name;
    bool is_unique{false};
    [[nodiscard]] StatementType type() const override { return StatementType::CreateIndex; }
};

struct DropTableStatement : Statement {
    std::string table_name;
    [[nodiscard]] StatementType type() const override { return StatementType::DropTable; }
};

struct DropIndexStatement : Statement {
    std::string index_name;
    std::string table_name; // optional table context
    [[nodiscard]] StatementType type() const override { return StatementType::DropIndex; }
};

struct InsertStatement : Statement {
    std::string table_name;
    std::vector<std::string> column_names; // empty for full positional insert
    std::vector<SqlValue> values;
    [[nodiscard]] StatementType type() const override { return StatementType::Insert; }
};

struct SelectStatement : Statement {
    std::string table_name;
    bool select_all{false};
    std::vector<std::string> column_names;
    WhereClause where;
    std::optional<OrderByClause> order_by;
    LimitClause limit;
    [[nodiscard]] StatementType type() const override { return StatementType::Select; }
};

struct SetClause {
    std::string column_name;
    SqlValue value;
};

struct UpdateStatement : Statement {
    std::string table_name;
    std::vector<SetClause> set_clauses;
    WhereClause where; // MANDATORY: UPDATE without WHERE is rejected
    [[nodiscard]] StatementType type() const override { return StatementType::Update; }
};

struct DeleteStatement : Statement {
    std::string table_name;
    WhereClause where; // MANDATORY: DELETE without WHERE is rejected
    [[nodiscard]] StatementType type() const override { return StatementType::Delete; }
};

struct BeginStatement : Statement {
    [[nodiscard]] StatementType type() const override { return StatementType::Begin; }
};

struct CommitStatement : Statement {
    [[nodiscard]] StatementType type() const override { return StatementType::Commit; }
};

struct RollbackStatement : Statement {
    [[nodiscard]] StatementType type() const override { return StatementType::Rollback; }
};

struct ExplainStatement : Statement {
    std::unique_ptr<Statement> query;
    bool is_query_plan{false};
    bool is_analyze{false};
    [[nodiscard]] StatementType type() const override { return StatementType::Explain; }
};

struct AnalyzeStatement : Statement {
    std::string table_name;
    [[nodiscard]] StatementType type() const override { return StatementType::Analyze; }
};

} // namespace klyrdb
