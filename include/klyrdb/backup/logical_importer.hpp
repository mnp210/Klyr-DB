#pragma once

#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include <filesystem>
#include <chrono>
#include <string>

namespace klyrdb {

struct ImportOptions {
    bool create_if_missing{true};
    bool replace_existing{false};
    bool stop_on_error{true};
    std::chrono::milliseconds lock_timeout{5000};
};

struct ImportResult {
    std::filesystem::path db_path;
    size_t statements_executed{0};
    size_t tables_imported{0};
    size_t rows_imported{0};
    double duration_ms{0.0};
};

/// Logical importer parsing SQL dump streams incrementally,
/// validating syntax, executing DDL and DML in safe transactions,
/// and providing complete rollback or staging disposal on failure.
class LogicalImporter {
public:
    LogicalImporter() = default;
    ~LogicalImporter() = default;

    /// Imports a logical SQL dump into the target database.
    Result<ImportResult> import_sql(
        const std::filesystem::path& input_path,
        const std::filesystem::path& db_path,
        const ImportOptions& options = {}) const;
};

} // namespace klyrdb
