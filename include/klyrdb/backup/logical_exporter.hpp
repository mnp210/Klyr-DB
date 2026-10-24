#pragma once

#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include <filesystem>
#include <chrono>
#include <string>

namespace klyrdb {

struct ExportOptions {
    bool include_schema{true};
    bool include_data{true};
    bool include_indexes{true};
    bool transactional{true};
    std::chrono::milliseconds lock_timeout{5000};
};

struct ExportResult {
    std::filesystem::path output_path;
    size_t tables_exported{0};
    size_t rows_exported{0};
    size_t indexes_exported{0};
    double duration_ms{0.0};
};

/// Logical exporter generating portable, human-readable standard SQL dumps
/// with dependency-aware DDL, transactional wrapping, and typed literal formatting.
class LogicalExporter {
public:
    LogicalExporter() = default;
    ~LogicalExporter() = default;

    /// Exports database schema and records to a SQL dump file.
    Result<ExportResult> export_sql(
        const std::filesystem::path& db_path,
        const std::filesystem::path& output_path,
        const ExportOptions& options = {}) const;
};

} // namespace klyrdb
