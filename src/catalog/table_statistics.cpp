#include "klyrdb/catalog/table_statistics.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/slotted_page.hpp"
#include <sstream>
#include <iomanip>
#include <set>
#include <unordered_set>
#include <chrono>
#include <algorithm>

namespace klyrdb {

std::string ColumnStatistics::to_string() const {
    std::ostringstream oss;
    oss << "  Column: " << column_name
        << " (Distinct: " << distinct_values
        << ", Nulls: " << null_count;
    if (min_value.has_value()) {
        oss << ", Min: " << min_value->to_string();
    }
    if (max_value.has_value()) {
        oss << ", Max: " << max_value->to_string();
    }
    oss << ")";
    return oss.str();
}

std::string ColumnStatistics::to_json() const {
    std::ostringstream oss;
    oss << "{\"column_name\":\"" << column_name << "\""
        << ",\"distinct_values\":" << distinct_values
        << ",\"null_count\":" << null_count;
    if (min_value.has_value()) {
        oss << ",\"min_value\":\"" << min_value->to_string() << "\"";
    } else {
        oss << ",\"min_value\":null";
    }
    if (max_value.has_value()) {
        oss << ",\"max_value\":\"" << max_value->to_string() << "\"";
    } else {
        oss << ",\"max_value\":null";
    }
    oss << "}";
    return oss.str();
}

const ColumnStatistics* TableStatistics::find_column_stats(std::string_view col_name) const noexcept {
    std::string lower = canonicalize_name(col_name);
    for (const auto& col : columns) {
        if (canonicalize_name(col.column_name) == lower) {
            return &col;
        }
    }
    return nullptr;
}

std::string TableStatistics::to_string() const {
    std::ostringstream oss;
    oss << "Table: " << table_name << " (ID: " << table_id << ")\n"
        << "  Total Rows:  " << total_rows << "\n"
        << "  Total Pages: " << total_pages << "\n"
        << "  Columns (" << columns.size() << "):\n";
    for (const auto& col : columns) {
        oss << "  " << col.to_string() << "\n";
    }
    return oss.str();
}

std::string TableStatistics::to_json() const {
    std::ostringstream oss;
    oss << "{\"table_name\":\"" << table_name << "\""
        << ",\"table_id\":" << table_id
        << ",\"total_rows\":" << total_rows
        << ",\"total_pages\":" << total_pages
        << ",\"last_analyzed_epoch_s\":" << last_analyzed_epoch_s
        << ",\"columns\":[";
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) oss << ",";
        oss << columns[i].to_json();
    }
    oss << "]}";
    return oss.str();
}

TableStatisticsRegistry& TableStatisticsRegistry::instance() noexcept {
    static TableStatisticsRegistry reg;
    return reg;
}

void TableStatisticsRegistry::set_statistics(std::string_view db_path, TableStatistics stats) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_[std::string(db_path)][stats.table_id] = std::move(stats);
}

std::optional<TableStatistics> TableStatisticsRegistry::get_statistics(std::string_view db_path, table_id_t table_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(std::string(db_path));
    if (it == cache_.end()) return std::nullopt;
    auto s_it = it->second.find(table_id);
    if (s_it == it->second.end()) return std::nullopt;
    return s_it->second;
}

std::optional<TableStatistics> TableStatisticsRegistry::get_statistics(std::string_view db_path, std::string_view table_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(std::string(db_path));
    if (it == cache_.end()) return std::nullopt;
    std::string lower = canonicalize_name(table_name);
    for (const auto& [tid, stats] : it->second) {
        if (canonicalize_name(stats.table_name) == lower) {
            return stats;
        }
    }
    return std::nullopt;
}

std::vector<TableStatistics> TableStatisticsRegistry::get_all_statistics(std::string_view db_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TableStatistics> res;
    auto it = cache_.find(std::string(db_path));
    if (it != cache_.end()) {
        for (const auto& [tid, stats] : it->second) {
            res.push_back(stats);
        }
    }
    return res;
}

void TableStatisticsRegistry::clear(std::string_view db_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.erase(std::string(db_path));
}

void TableStatisticsRegistry::clear_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

Result<TableStatistics> analyze_table(BufferPoolManager& bpm, RecordManager& rm, const TableSchema& schema) {
    (void)rm;
    TableStatistics stats;
    stats.table_name = schema.name;
    stats.table_id = schema.table_id;

    const auto now = std::chrono::system_clock::now();
    stats.last_analyzed_epoch_s = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count()
    );

    const size_t num_cols = schema.columns.size();
    stats.columns.resize(num_cols);
    for (size_t i = 0; i < num_cols; ++i) {
        stats.columns[i].column_name = schema.columns[i].name;
    }

    std::vector<std::set<SqlValue>> distinct_sets(num_cols);
    std::unordered_set<page_id_t> pages_with_table_records;

    const auto expected_types = schema.column_types();
    const uint32_t total_pages = bpm.page_file().page_count();

    for (page_id_t pid = 1; pid < total_pages; ++pid) {
        auto guard_res = bpm.fetch_page_guard(pid);
        if (guard_res.is_err()) continue;

        auto& guard = guard_res.value();
        if (guard->page_type() != PageType::Record || !SlottedPage::is_slotted_page(*guard)) {
            continue;
        }

        SlottedPage slotted(*guard);
        const uint16_t slot_cnt = slotted.slot_count();
        bool page_had_table_record = false;

        for (slot_id_t slot = 0; slot < slot_cnt; ++slot) {
            auto span_res = slotted.get_record(slot);
            if (span_res.is_err()) continue;

            auto peek_res = SqlRow::peek_table_id(span_res.value());
            if (peek_res.is_err() || peek_res.value() != schema.table_id) {
                continue;
            }

            auto row_res = SqlRow::decode(span_res.value(), schema.table_id, expected_types);
            if (row_res.is_err()) continue;

            page_had_table_record = true;
            stats.total_rows++;
            const auto& row = row_res.value();

            for (size_t col_idx = 0; col_idx < num_cols; ++col_idx) {
                const auto& val = row.get(col_idx);
                if (val.is_null()) {
                    stats.columns[col_idx].null_count++;
                } else {
                    distinct_sets[col_idx].insert(val);

                    if (!stats.columns[col_idx].min_value.has_value() || val < *stats.columns[col_idx].min_value) {
                        stats.columns[col_idx].min_value = val;
                    }
                    if (!stats.columns[col_idx].max_value.has_value() || val > *stats.columns[col_idx].max_value) {
                        stats.columns[col_idx].max_value = val;
                    }
                }
            }
        }

        if (page_had_table_record) {
            pages_with_table_records.insert(pid);
        }
    }

    stats.total_pages = pages_with_table_records.size();

    for (size_t i = 0; i < num_cols; ++i) {
        stats.columns[i].distinct_values = distinct_sets[i].size();
    }

    TableStatisticsRegistry::instance().set_statistics(bpm.page_file().path().string(), stats);
    return stats;
}

Result<std::vector<TableStatistics>> analyze_database(BufferPoolManager& bpm, RecordManager& rm, const TableCatalog& tc) {
    std::vector<TableStatistics> results;
    auto tables = tc.list_tables();
    for (const auto& tbl : tables) {
        auto res = analyze_table(bpm, rm, tbl);
        if (res.is_err()) return res.status();
        results.push_back(std::move(res.value()));
    }
    return results;
}

} // namespace klyrdb
