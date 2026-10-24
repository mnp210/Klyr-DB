#pragma once

#include <cstdint>
#include <string>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <filesystem>
#include <chrono>

namespace klyrdb {

/// Snapshot of buffer pool operations and caching efficiency.
struct BufferPoolMetricsSnapshot {
    uint64_t page_requests{0};
    uint64_t cache_hits{0};
    uint64_t cache_misses{0};
    uint64_t physical_reads{0};
    uint64_t physical_writes{0};
    uint64_t page_flushes{0};
    uint64_t evictions{0};
    uint64_t allocation_failures{0};
    uint64_t current_pinned_frames{0};
    uint64_t peak_pinned_frames{0};

    [[nodiscard]] double hit_ratio() const noexcept {
        return page_requests > 0 ? (static_cast<double>(cache_hits) / static_cast<double>(page_requests)) : 0.0;
    }
};

/// Snapshot of query execution throughput and access path breakdown.
struct QueryMetricsSnapshot {
    uint64_t statements_executed{0};
    uint64_t statements_successful{0};
    uint64_t statements_failed{0};
    uint64_t rows_examined{0};
    uint64_t rows_returned{0};
    uint64_t rows_inserted{0};
    uint64_t rows_updated{0};
    uint64_t rows_deleted{0};
    uint64_t table_scans{0};
    uint64_t index_seeks{0};
    uint64_t index_range_scans{0};
    uint64_t sort_operations{0};
    uint64_t execution_duration_us{0};
};

/// Snapshot of transactional concurrency and rollback statistics.
struct TransactionMetricsSnapshot {
    uint64_t transactions_started{0};
    uint64_t transactions_committed{0};
    uint64_t transactions_aborted{0};
    uint64_t rollbacks_executed{0};
    uint64_t commit_failures{0};
    uint64_t lock_waits{0};
    uint64_t lock_timeouts{0};
    uint64_t lock_wait_duration_us{0};
};

/// Snapshot of Write-Ahead Logging, checkpointing, and crash recovery.
struct WalRecoveryMetricsSnapshot {
    uint64_t wal_bytes_written{0};
    uint64_t wal_bytes_flushed{0};
    uint64_t wal_records_written{0};
    uint64_t wal_records_scanned{0};
    uint64_t checkpoints_attempted{0};
    uint64_t checkpoints_succeeded{0};
    uint64_t checkpoint_duration_us{0};
    uint64_t recoveries_attempted{0};
    uint64_t recoveries_succeeded{0};
    uint64_t recovery_duration_us{0};
    uint64_t pages_redone{0};
    uint64_t pages_undone{0};
};

/// Snapshot of physical backups, verification, and migration data volumes.
struct BackupMigrationMetricsSnapshot {
    uint64_t backup_attempts{0};
    uint64_t backup_successes{0};
    uint64_t backup_bytes{0};
    uint64_t backup_duration_us{0};
    uint64_t verification_attempts{0};
    uint64_t verification_failures{0};
    uint64_t restore_attempts{0};
    uint64_t restore_successes{0};
    uint64_t restore_duration_us{0};
    uint64_t sql_export_rows{0};
    uint64_t sql_import_rows{0};
    uint64_t csv_export_rows{0};
    uint64_t csv_import_rows{0};
};

/// Aggregate immutable snapshot of all metrics at a specific point in time.
struct DatabaseMetricsSnapshot {
    std::string database_name;
    uint64_t timestamp_utc_epoch_s{0};
    BufferPoolMetricsSnapshot buffer_pool;
    QueryMetricsSnapshot queries;
    TransactionMetricsSnapshot transactions;
    WalRecoveryMetricsSnapshot wal_recovery;
    BackupMigrationMetricsSnapshot backup_migration;

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] std::string to_json() const;
};

/// Live, thread-safe, atomic metrics accumulator for a database instance.
class DatabaseMetrics {
public:
    explicit DatabaseMetrics(std::string database_name = "") noexcept;
    ~DatabaseMetrics() = default;

    DatabaseMetrics(const DatabaseMetrics&) = delete;
    DatabaseMetrics& operator=(const DatabaseMetrics&) = delete;
    DatabaseMetrics(DatabaseMetrics&&) = delete;
    DatabaseMetrics& operator=(DatabaseMetrics&&) = delete;

    // --- Buffer Pool Recording ---
    void record_page_request() noexcept { page_requests_.fetch_add(1, std::memory_order_relaxed); }
    void record_cache_hit() noexcept { cache_hits_.fetch_add(1, std::memory_order_relaxed); }
    void record_cache_miss() noexcept { cache_misses_.fetch_add(1, std::memory_order_relaxed); }
    void record_physical_read() noexcept { physical_reads_.fetch_add(1, std::memory_order_relaxed); }
    void record_physical_write() noexcept { physical_writes_.fetch_add(1, std::memory_order_relaxed); }
    void record_page_flush() noexcept { page_flushes_.fetch_add(1, std::memory_order_relaxed); }
    void record_eviction() noexcept { evictions_.fetch_add(1, std::memory_order_relaxed); }
    void record_allocation_failure() noexcept { allocation_failures_.fetch_add(1, std::memory_order_relaxed); }
    void update_pinned_frames(size_t current) noexcept;

    // --- Query Execution Recording ---
    void record_statement_executed(bool success, uint64_t duration_us) noexcept;
    void record_rows_examined(size_t count) noexcept { rows_examined_.fetch_add(count, std::memory_order_relaxed); }
    void record_rows_returned(size_t count) noexcept { rows_returned_.fetch_add(count, std::memory_order_relaxed); }
    void record_rows_inserted(size_t count) noexcept { rows_inserted_.fetch_add(count, std::memory_order_relaxed); }
    void record_rows_updated(size_t count) noexcept { rows_updated_.fetch_add(count, std::memory_order_relaxed); }
    void record_rows_deleted(size_t count) noexcept { rows_deleted_.fetch_add(count, std::memory_order_relaxed); }
    void record_table_scan() noexcept { table_scans_.fetch_add(1, std::memory_order_relaxed); }
    void record_index_seek() noexcept { index_seeks_.fetch_add(1, std::memory_order_relaxed); }
    void record_index_range_scan() noexcept { index_range_scans_.fetch_add(1, std::memory_order_relaxed); }
    void record_sort_operation() noexcept { sort_operations_.fetch_add(1, std::memory_order_relaxed); }

    // --- Transaction Recording ---
    void record_transaction_started() noexcept { transactions_started_.fetch_add(1, std::memory_order_relaxed); }
    void record_transaction_committed() noexcept { transactions_committed_.fetch_add(1, std::memory_order_relaxed); }
    void record_transaction_aborted() noexcept { transactions_aborted_.fetch_add(1, std::memory_order_relaxed); }
    void record_rollback() noexcept { rollbacks_executed_.fetch_add(1, std::memory_order_relaxed); }
    void record_commit_failure() noexcept { commit_failures_.fetch_add(1, std::memory_order_relaxed); }
    void record_lock_wait(uint64_t duration_us) noexcept;
    void record_lock_timeout() noexcept { lock_timeouts_.fetch_add(1, std::memory_order_relaxed); }

    // --- WAL & Recovery Recording ---
    void record_wal_write(size_t bytes) noexcept;
    void record_wal_flush(size_t bytes) noexcept;
    void record_wal_records_scanned(size_t count) noexcept { wal_records_scanned_.fetch_add(count, std::memory_order_relaxed); }
    void record_checkpoint(bool success, uint64_t duration_us) noexcept;
    void record_recovery(bool success, uint64_t duration_us, size_t redone, size_t undone) noexcept;

    // --- Backup & Migration Recording ---
    void record_backup(bool success, size_t bytes, uint64_t duration_us) noexcept;
    void record_verification(bool success) noexcept;
    void record_restore(bool success, uint64_t duration_us) noexcept;
    void record_sql_export(size_t rows) noexcept { sql_export_rows_.fetch_add(rows, std::memory_order_relaxed); }
    void record_sql_import(size_t rows) noexcept { sql_import_rows_.fetch_add(rows, std::memory_order_relaxed); }
    void record_csv_export(size_t rows) noexcept { csv_export_rows_.fetch_add(rows, std::memory_order_relaxed); }
    void record_csv_import(size_t rows) noexcept { csv_import_rows_.fetch_add(rows, std::memory_order_relaxed); }

    /// Captures a thread-safe point-in-time snapshot.
    [[nodiscard]] DatabaseMetricsSnapshot snapshot() const noexcept;

    /// Resets all counters to zero.
    void reset() noexcept;

    [[nodiscard]] const std::string& database_name() const noexcept { return database_name_; }
    void set_database_name(std::string name) { database_name_ = std::move(name); }

private:
    std::string database_name_;

    // Buffer Pool
    std::atomic<uint64_t> page_requests_{0};
    std::atomic<uint64_t> cache_hits_{0};
    std::atomic<uint64_t> cache_misses_{0};
    std::atomic<uint64_t> physical_reads_{0};
    std::atomic<uint64_t> physical_writes_{0};
    std::atomic<uint64_t> page_flushes_{0};
    std::atomic<uint64_t> evictions_{0};
    std::atomic<uint64_t> allocation_failures_{0};
    std::atomic<uint64_t> current_pinned_frames_{0};
    std::atomic<uint64_t> peak_pinned_frames_{0};

    // Query Execution
    std::atomic<uint64_t> statements_executed_{0};
    std::atomic<uint64_t> statements_successful_{0};
    std::atomic<uint64_t> statements_failed_{0};
    std::atomic<uint64_t> rows_examined_{0};
    std::atomic<uint64_t> rows_returned_{0};
    std::atomic<uint64_t> rows_inserted_{0};
    std::atomic<uint64_t> rows_updated_{0};
    std::atomic<uint64_t> rows_deleted_{0};
    std::atomic<uint64_t> table_scans_{0};
    std::atomic<uint64_t> index_seeks_{0};
    std::atomic<uint64_t> index_range_scans_{0};
    std::atomic<uint64_t> sort_operations_{0};
    std::atomic<uint64_t> execution_duration_us_{0};

    // Transactions
    std::atomic<uint64_t> transactions_started_{0};
    std::atomic<uint64_t> transactions_committed_{0};
    std::atomic<uint64_t> transactions_aborted_{0};
    std::atomic<uint64_t> rollbacks_executed_{0};
    std::atomic<uint64_t> commit_failures_{0};
    std::atomic<uint64_t> lock_waits_{0};
    std::atomic<uint64_t> lock_timeouts_{0};
    std::atomic<uint64_t> lock_wait_duration_us_{0};

    // WAL & Recovery
    std::atomic<uint64_t> wal_bytes_written_{0};
    std::atomic<uint64_t> wal_bytes_flushed_{0};
    std::atomic<uint64_t> wal_records_written_{0};
    std::atomic<uint64_t> wal_records_scanned_{0};
    std::atomic<uint64_t> checkpoints_attempted_{0};
    std::atomic<uint64_t> checkpoints_succeeded_{0};
    std::atomic<uint64_t> checkpoint_duration_us_{0};
    std::atomic<uint64_t> recoveries_attempted_{0};
    std::atomic<uint64_t> recoveries_succeeded_{0};
    std::atomic<uint64_t> recovery_duration_us_{0};
    std::atomic<uint64_t> pages_redone_{0};
    std::atomic<uint64_t> pages_undone_{0};

    // Backup & Migration
    std::atomic<uint64_t> backup_attempts_{0};
    std::atomic<uint64_t> backup_successes_{0};
    std::atomic<uint64_t> backup_bytes_{0};
    std::atomic<uint64_t> backup_duration_us_{0};
    std::atomic<uint64_t> verification_attempts_{0};
    std::atomic<uint64_t> verification_failures_{0};
    std::atomic<uint64_t> restore_attempts_{0};
    std::atomic<uint64_t> restore_successes_{0};
    std::atomic<uint64_t> restore_duration_us_{0};
    std::atomic<uint64_t> sql_export_rows_{0};
    std::atomic<uint64_t> sql_import_rows_{0};
    std::atomic<uint64_t> csv_export_rows_{0};
    std::atomic<uint64_t> csv_import_rows_{0};
};

/// Registry providing thread-safe retrieval and mapping of metrics per database path.
class DatabaseMetricsRegistry {
public:
    static DatabaseMetricsRegistry& instance() noexcept;

    /// Retrieves or registers the DatabaseMetrics instance for a given database path.
    [[nodiscard]] std::shared_ptr<DatabaseMetrics> get_metrics(const std::filesystem::path& db_path);

    /// Resets metrics for a specific database.
    void reset_metrics(const std::filesystem::path& db_path);

    /// Clears any unreferenced metrics instances.
    void clean_unused();

private:
    DatabaseMetricsRegistry() = default;
    ~DatabaseMetricsRegistry() = default;

    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<DatabaseMetrics>> registry_;
};

} // namespace klyrdb
