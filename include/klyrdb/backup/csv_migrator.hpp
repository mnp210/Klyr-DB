#pragma once

#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include <filesystem>
#include <chrono>
#include <string>

namespace klyrdb {

struct CsvExportOptions {
    bool include_header{true};
    std::string null_string{"\\N"};
    std::chrono::milliseconds lock_timeout{5000};
};

struct CsvImportOptions {
    bool has_header{true};
    std::string null_string{"\\N"};
    bool stop_on_error{true};
    std::chrono::milliseconds lock_timeout{5000};
};

struct CsvResult {
    size_t rows_processed{0};
    double duration_ms{0.0};
};

/// High-fidelity RFC 4180 CSV export and import engine
/// providing explicit NULL vs empty string differentiation,
/// streaming parsing with embedded newline support, and constraint enforcement.
class CsvMigrator {
public:
    CsvMigrator() = default;
    ~CsvMigrator() = default;

    /// Exports rows from a table into a CSV file.
    Result<CsvResult> export_csv(
        const std::filesystem::path& db_path,
        const std::string& table_name,
        const std::filesystem::path& output_path,
        const CsvExportOptions& options = {}) const;

    /// Imports records from a CSV file into an existing compatible table.
    Result<CsvResult> import_csv(
        const std::filesystem::path& input_path,
        const std::filesystem::path& db_path,
        const std::string& table_name,
        const CsvImportOptions& options = {}) const;
};

} // namespace klyrdb
