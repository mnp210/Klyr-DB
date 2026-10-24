#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/sql/value.hpp"
#include "klyrdb/sql/row.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <optional>
#include <cctype>

namespace klyrdb {

/// Helper to canonicalize identifiers to lowercase.
[[nodiscard]] inline std::string canonicalize_name(std::string_view name) {
    std::string res;
    res.reserve(name.size());
    for (char c : name) {
        res.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return res;
}

/// Validates that an identifier consists of valid SQL identifier characters
/// (begins with letter/underscore, followed by letters, digits, underscores).
[[nodiscard]] inline Status validate_identifier(std::string_view name, std::string_view entity_type = "Identifier") {
    if (name.empty()) {
        return Status::invalid_argument(std::string(entity_type) + " cannot be empty");
    }
    char first = name.front();
    if (!std::isalpha(static_cast<unsigned char>(first)) && first != '_') {
        return Status::invalid_argument(std::string(entity_type) + " '" + std::string(name) +
                                        "' must begin with a letter or underscore");
    }
    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            return Status::invalid_argument(std::string(entity_type) + " '" + std::string(name) +
                                            "' contains invalid character: '" + c + "'");
        }
    }
    return Status::ok();
}

/// Supported constraint types in KlyrDB Phase 6.
enum class ConstraintType : uint8_t {
    NotNull    = 0x01,
    PrimaryKey = 0x02,
    Unique     = 0x03,
    ForeignKey = 0x04
};

[[nodiscard]] inline std::string to_string(ConstraintType ct) {
    switch (ct) {
        case ConstraintType::NotNull:    return "NOT NULL";
        case ConstraintType::PrimaryKey: return "PRIMARY KEY";
        case ConstraintType::Unique:     return "UNIQUE";
        case ConstraintType::ForeignKey: return "FOREIGN KEY";
    }
    return "UNKNOWN";
}

/// Relational table constraint definition.
struct TableConstraint {
    ConstraintType type{ConstraintType::NotNull};
    std::string name;                   // unique constraint identifier (lowercase)
    std::string column_name;            // constrained column name (lowercase)
    std::string ref_table_name;         // referenced parent table for Foreign Key (lowercase)
    std::string ref_column_name;        // referenced parent column for Foreign Key (lowercase)
    std::string enforcement_index_name; // backing unique index name (lowercase)
};

/// Column definition within a table schema.
struct ColumnDef {
    std::string name;
    SqlType type{SqlType::Integer};
};

/// Attached index descriptor within a table schema.
struct TableIndexInfo {
    std::string index_name;
    std::string column_name;
    size_t column_index{0};
    page_id_t root_page_id{INVALID_PAGE_ID};
    bool is_unique{false};
};

/// Complete schema definition of a table in KlyrDB.
struct TableSchema {
    table_id_t table_id{INVALID_TABLE_ID};
    std::string name;
    std::vector<ColumnDef> columns;
    std::vector<TableIndexInfo> indexes;
    std::vector<TableConstraint> constraints;

    [[nodiscard]] bool is_not_null(std::string_view col_name) const {
        std::string lower = canonicalize_name(col_name);
        for (const auto& c : constraints) {
            if (c.column_name == lower && (c.type == ConstraintType::NotNull || c.type == ConstraintType::PrimaryKey)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool is_primary_key(std::string_view col_name) const {
        std::string lower = canonicalize_name(col_name);
        for (const auto& c : constraints) {
            if (c.column_name == lower && c.type == ConstraintType::PrimaryKey) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool is_unique(std::string_view col_name) const {
        std::string lower = canonicalize_name(col_name);
        for (const auto& c : constraints) {
            if (c.column_name == lower && (c.type == ConstraintType::Unique || c.type == ConstraintType::PrimaryKey)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] const TableConstraint* find_primary_key() const {
        for (const auto& c : constraints) {
            if (c.type == ConstraintType::PrimaryKey) {
                return &c;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::vector<const TableConstraint*> find_foreign_keys() const {
        std::vector<const TableConstraint*> fks;
        for (const auto& c : constraints) {
            if (c.type == ConstraintType::ForeignKey) {
                fks.push_back(&c);
            }
        }
        return fks;
    }

    [[nodiscard]] const TableConstraint* find_constraint_by_name(std::string_view c_name) const {
        std::string lower = canonicalize_name(c_name);
        for (const auto& c : constraints) {
            if (c.name == lower) {
                return &c;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::optional<size_t> find_column_index(std::string_view col_name) const {
        std::string lower = canonicalize_name(col_name);
        for (size_t i = 0; i < columns.size(); ++i) {
            if (columns[i].name == lower) {
                return i;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] const ColumnDef* find_column(std::string_view col_name) const {
        auto idx = find_column_index(col_name);
        if (idx.has_value()) {
            return &columns[*idx];
        }
        return nullptr;
    }

    [[nodiscard]] const TableIndexInfo* find_index_for_column(std::string_view col_name) const {
        std::string lower = canonicalize_name(col_name);
        for (const auto& idx : indexes) {
            if (idx.column_name == lower) {
                return &idx;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const TableIndexInfo* find_index_by_name(std::string_view idx_name) const {
        std::string lower = canonicalize_name(idx_name);
        for (const auto& idx : indexes) {
            if (idx.index_name == lower) {
                return &idx;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::vector<SqlType> column_types() const {
        std::vector<SqlType> types;
        types.reserve(columns.size());
        for (const auto& col : columns) {
            types.push_back(col.type);
        }
        return types;
    }
};

/// Persistent catalog tracking table schemas, columns, and attached indexes across database sessions.
class TableCatalog {
public:
    explicit TableCatalog(BufferPoolManager& bpm, page_id_t catalog_page_id) noexcept
        : bpm_(bpm), catalog_page_id_(catalog_page_id) {}

    /// Opens an existing TableCatalog from Page 0 or initializes a new one.
    static Result<std::unique_ptr<TableCatalog>> open_or_create(BufferPoolManager& bpm);

    [[nodiscard]] page_id_t catalog_page_id() const noexcept { return catalog_page_id_; }

    /// Registers a new table with the specified columns and constraints.
    Result<TableSchema> create_table(std::string_view table_name,
                                     std::vector<ColumnDef> columns,
                                     std::vector<TableConstraint> constraints = {});

    /// Looks up a table schema by name (case-insensitive).
    [[nodiscard]] Result<TableSchema> get_table(std::string_view table_name) const;

    /// Looks up a table schema by table_id.
    [[nodiscard]] Result<TableSchema> get_table(table_id_t table_id) const;

    /// Checks if a table exists (case-insensitive).
    [[nodiscard]] bool has_table(std::string_view table_name) const noexcept;

    /// Drops a table and its registered indexes from the catalog.
    Status drop_table(std::string_view table_name);

    /// Attaches an index descriptor to an existing table.
    Status add_index_to_table(std::string_view table_name, TableIndexInfo index_info);

    /// Updates the root page ID for an index attached to a table.
    Status update_index_root(std::string_view table_name, std::string_view index_name, page_id_t new_root_id);

    /// Removes an index from a table schema.
    Status drop_index_from_table(std::string_view table_name, std::string_view index_name);

    /// Lists all active tables in the catalog.
    [[nodiscard]] std::vector<TableSchema> list_tables() const;

private:
    Status load_from_page();
    Status persist_to_page();

    BufferPoolManager& bpm_;
    page_id_t catalog_page_id_{INVALID_PAGE_ID};
    table_id_t last_table_id_{0};
    std::vector<TableSchema> tables_;
};

} // namespace klyrdb
