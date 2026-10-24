#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/sql/value.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <optional>
#include <memory>
#include <mutex>
#include <cstdint>

namespace klyrdb {

class BufferPoolManager;
class RecordManager;

/// Detailed statistical distribution of a single column.
struct ColumnStatistics {
    std::string column_name;
    size_t distinct_values{0};
    size_t null_count{0};
    std::optional<SqlValue> min_value;
    std::optional<SqlValue> max_value;

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] std::string to_json() const;
};

/// Comprehensive statistical profile of a persistent table.
struct TableStatistics {
    std::string table_name;
    table_id_t table_id{INVALID_TABLE_ID};
    size_t total_rows{0};
    size_t total_pages{0};
    uint64_t last_analyzed_epoch_s{0};
    std::vector<ColumnStatistics> columns;

    [[nodiscard]] const ColumnStatistics* find_column_stats(std::string_view col_name) const noexcept;
    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] std::string to_json() const;
};

/// Global or per-database statistics cache to accelerate query planning.
class TableStatisticsRegistry {
public:
    static TableStatisticsRegistry& instance() noexcept;

    void set_statistics(std::string_view db_path, TableStatistics stats);
    [[nodiscard]] std::optional<TableStatistics> get_statistics(std::string_view db_path, table_id_t table_id) const;
    [[nodiscard]] std::optional<TableStatistics> get_statistics(std::string_view db_path, std::string_view table_name) const;
    [[nodiscard]] std::vector<TableStatistics> get_all_statistics(std::string_view db_path) const;
    void clear(std::string_view db_path);
    void clear_all();

private:
    TableStatisticsRegistry() = default;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unordered_map<table_id_t, TableStatistics>> cache_;
};

/// Analyzes a specific table by scanning its records and calculating cardinality and distribution.
Result<TableStatistics> analyze_table(BufferPoolManager& bpm, RecordManager& rm, const TableSchema& schema);

/// Analyzes all active tables registered in the catalog.
Result<std::vector<TableStatistics>> analyze_database(BufferPoolManager& bpm, RecordManager& rm, const TableCatalog& tc);

} // namespace klyrdb
