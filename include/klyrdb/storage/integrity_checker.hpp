#pragma once

#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/common/types.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/concurrency/database_lock.hpp"
#include <string>
#include <vector>
#include <memory>
#include <iostream>

namespace klyrdb {

enum class IntegritySeverity {
    Info,
    Warning,
    Error
};

[[nodiscard]] inline const char* to_string(IntegritySeverity s) noexcept {
    switch (s) {
        case IntegritySeverity::Info:    return "INFO";
        case IntegritySeverity::Warning: return "WARNING";
        case IntegritySeverity::Error:   return "ERROR";
    }
    return "UNKNOWN";
}

struct IntegrityIssue {
    IntegritySeverity severity{IntegritySeverity::Error};
    std::string subsystem;
    page_id_t page_id{INVALID_PAGE_ID};
    std::string object_name;
    std::string message;
};

struct IntegrityReport {
    bool is_healthy{true};
    size_t total_pages_checked{0};
    size_t pages_corrupted{0};
    size_t tables_checked{0};
    size_t indexes_checked{0};
    size_t records_checked{0};
    size_t fsm_pages_checked{0};
    bool wal_checked{false};
    bool wal_healthy{true};
    std::vector<IntegrityIssue> issues;

    [[nodiscard]] size_t error_count() const noexcept {
        size_t c = 0;
        for (const auto& issue : issues) {
            if (issue.severity == IntegritySeverity::Error) c++;
        }
        return c;
    }

    [[nodiscard]] size_t warning_count() const noexcept {
        size_t c = 0;
        for (const auto& issue : issues) {
            if (issue.severity == IntegritySeverity::Warning) c++;
        }
        return c;
    }

    void add_error(std::string subsystem, std::string message,
                   page_id_t pid = INVALID_PAGE_ID, std::string obj = "") {
        is_healthy = false;
        issues.push_back(IntegrityIssue{IntegritySeverity::Error, std::move(subsystem), pid, std::move(obj), std::move(message)});
    }

    void add_warning(std::string subsystem, std::string message,
                     page_id_t pid = INVALID_PAGE_ID, std::string obj = "") {
        issues.push_back(IntegrityIssue{IntegritySeverity::Warning, std::move(subsystem), pid, std::move(obj), std::move(message)});
    }

    void print(std::ostream& os) const;
};

/// Read-only database integrity-check engine auditing page checksums, page types,
/// slotted-page bounds, B+ tree invariants, schema constraints, FSM, and WAL.
class IntegrityChecker {
public:
    explicit IntegrityChecker(PageFile& page_file, WriteAheadLog* wal = nullptr) noexcept;
    ~IntegrityChecker() = default;

    IntegrityChecker(const IntegrityChecker&) = delete;
    IntegrityChecker& operator=(const IntegrityChecker&) = delete;
    IntegrityChecker(IntegrityChecker&&) = default;
    IntegrityChecker& operator=(IntegrityChecker&&) = default;

    /// Runs all verification suites and returns a detailed report.
    Result<IntegrityReport> check_all();

private:
    void check_header(IntegrityReport& report);
    void check_pages_and_checksums(IntegrityReport& report);
    void check_slotted_pages(IntegrityReport& report);
    void check_catalogs_and_indexes(IntegrityReport& report);
    void check_foreign_keys(IntegrityReport& report);
    void check_free_space_map(IntegrityReport& report);
    void check_wal(IntegrityReport& report);

    PageFile& page_file_;
    WriteAheadLog* wal_{nullptr};
    std::shared_ptr<DatabaseLock> db_lock_;
};

} // namespace klyrdb
