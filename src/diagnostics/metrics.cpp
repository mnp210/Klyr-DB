#include "klyrdb/diagnostics/metrics.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace klyrdb {

DatabaseMetrics::DatabaseMetrics(std::string database_name) noexcept
    : database_name_(std::move(database_name)) {}

void DatabaseMetrics::update_pinned_frames(size_t current) noexcept {
    current_pinned_frames_.store(current, std::memory_order_relaxed);
    uint64_t prev_peak = peak_pinned_frames_.load(std::memory_order_relaxed);
    while (current > prev_peak && !peak_pinned_frames_.compare_exchange_weak(
               prev_peak, current, std::memory_order_relaxed, std::memory_order_relaxed)) {
        // Retry loop until peak is >= current
    }
}

void DatabaseMetrics::record_statement_executed(bool success, uint64_t duration_us) noexcept {
    statements_executed_.fetch_add(1, std::memory_order_relaxed);
    if (success) {
        statements_successful_.fetch_add(1, std::memory_order_relaxed);
    } else {
        statements_failed_.fetch_add(1, std::memory_order_relaxed);
    }
    execution_duration_us_.fetch_add(duration_us, std::memory_order_relaxed);
}

void DatabaseMetrics::record_lock_wait(uint64_t duration_us) noexcept {
    lock_waits_.fetch_add(1, std::memory_order_relaxed);
    lock_wait_duration_us_.fetch_add(duration_us, std::memory_order_relaxed);
}

void DatabaseMetrics::record_wal_write(size_t bytes) noexcept {
    wal_bytes_written_.fetch_add(bytes, std::memory_order_relaxed);
    wal_records_written_.fetch_add(1, std::memory_order_relaxed);
}

void DatabaseMetrics::record_wal_flush(size_t bytes) noexcept {
    wal_bytes_flushed_.fetch_add(bytes, std::memory_order_relaxed);
}

void DatabaseMetrics::record_checkpoint(bool success, uint64_t duration_us) noexcept {
    checkpoints_attempted_.fetch_add(1, std::memory_order_relaxed);
    if (success) {
        checkpoints_succeeded_.fetch_add(1, std::memory_order_relaxed);
    }
    checkpoint_duration_us_.fetch_add(duration_us, std::memory_order_relaxed);
}

void DatabaseMetrics::record_recovery(bool success, uint64_t duration_us, size_t redone, size_t undone) noexcept {
    recoveries_attempted_.fetch_add(1, std::memory_order_relaxed);
    if (success) {
        recoveries_succeeded_.fetch_add(1, std::memory_order_relaxed);
    }
    recovery_duration_us_.fetch_add(duration_us, std::memory_order_relaxed);
    pages_redone_.fetch_add(redone, std::memory_order_relaxed);
    pages_undone_.fetch_add(undone, std::memory_order_relaxed);
}

void DatabaseMetrics::record_backup(bool success, size_t bytes, uint64_t duration_us) noexcept {
    backup_attempts_.fetch_add(1, std::memory_order_relaxed);
    if (success) {
        backup_successes_.fetch_add(1, std::memory_order_relaxed);
        backup_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    }
    backup_duration_us_.fetch_add(duration_us, std::memory_order_relaxed);
}

void DatabaseMetrics::record_verification(bool success) noexcept {
    verification_attempts_.fetch_add(1, std::memory_order_relaxed);
    if (!success) {
        verification_failures_.fetch_add(1, std::memory_order_relaxed);
    }
}

void DatabaseMetrics::record_restore(bool success, uint64_t duration_us) noexcept {
    restore_attempts_.fetch_add(1, std::memory_order_relaxed);
    if (success) {
        restore_successes_.fetch_add(1, std::memory_order_relaxed);
    }
    restore_duration_us_.fetch_add(duration_us, std::memory_order_relaxed);
}

DatabaseMetricsSnapshot DatabaseMetrics::snapshot() const noexcept {
    DatabaseMetricsSnapshot snap;
    snap.database_name = database_name_;
    snap.timestamp_utc_epoch_s = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // Buffer Pool
    snap.buffer_pool.page_requests = page_requests_.load(std::memory_order_relaxed);
    snap.buffer_pool.cache_hits = cache_hits_.load(std::memory_order_relaxed);
    snap.buffer_pool.cache_misses = cache_misses_.load(std::memory_order_relaxed);
    snap.buffer_pool.physical_reads = physical_reads_.load(std::memory_order_relaxed);
    snap.buffer_pool.physical_writes = physical_writes_.load(std::memory_order_relaxed);
    snap.buffer_pool.page_flushes = page_flushes_.load(std::memory_order_relaxed);
    snap.buffer_pool.evictions = evictions_.load(std::memory_order_relaxed);
    snap.buffer_pool.allocation_failures = allocation_failures_.load(std::memory_order_relaxed);
    snap.buffer_pool.current_pinned_frames = current_pinned_frames_.load(std::memory_order_relaxed);
    snap.buffer_pool.peak_pinned_frames = peak_pinned_frames_.load(std::memory_order_relaxed);

    // Queries
    snap.queries.statements_executed = statements_executed_.load(std::memory_order_relaxed);
    snap.queries.statements_successful = statements_successful_.load(std::memory_order_relaxed);
    snap.queries.statements_failed = statements_failed_.load(std::memory_order_relaxed);
    snap.queries.rows_examined = rows_examined_.load(std::memory_order_relaxed);
    snap.queries.rows_returned = rows_returned_.load(std::memory_order_relaxed);
    snap.queries.rows_inserted = rows_inserted_.load(std::memory_order_relaxed);
    snap.queries.rows_updated = rows_updated_.load(std::memory_order_relaxed);
    snap.queries.rows_deleted = rows_deleted_.load(std::memory_order_relaxed);
    snap.queries.table_scans = table_scans_.load(std::memory_order_relaxed);
    snap.queries.index_seeks = index_seeks_.load(std::memory_order_relaxed);
    snap.queries.index_range_scans = index_range_scans_.load(std::memory_order_relaxed);
    snap.queries.sort_operations = sort_operations_.load(std::memory_order_relaxed);
    snap.queries.execution_duration_us = execution_duration_us_.load(std::memory_order_relaxed);

    // Transactions
    snap.transactions.transactions_started = transactions_started_.load(std::memory_order_relaxed);
    snap.transactions.transactions_committed = transactions_committed_.load(std::memory_order_relaxed);
    snap.transactions.transactions_aborted = transactions_aborted_.load(std::memory_order_relaxed);
    snap.transactions.rollbacks_executed = rollbacks_executed_.load(std::memory_order_relaxed);
    snap.transactions.commit_failures = commit_failures_.load(std::memory_order_relaxed);
    snap.transactions.lock_waits = lock_waits_.load(std::memory_order_relaxed);
    snap.transactions.lock_timeouts = lock_timeouts_.load(std::memory_order_relaxed);
    snap.transactions.lock_wait_duration_us = lock_wait_duration_us_.load(std::memory_order_relaxed);

    // WAL & Recovery
    snap.wal_recovery.wal_bytes_written = wal_bytes_written_.load(std::memory_order_relaxed);
    snap.wal_recovery.wal_bytes_flushed = wal_bytes_flushed_.load(std::memory_order_relaxed);
    snap.wal_recovery.wal_records_written = wal_records_written_.load(std::memory_order_relaxed);
    snap.wal_recovery.wal_records_scanned = wal_records_scanned_.load(std::memory_order_relaxed);
    snap.wal_recovery.checkpoints_attempted = checkpoints_attempted_.load(std::memory_order_relaxed);
    snap.wal_recovery.checkpoints_succeeded = checkpoints_succeeded_.load(std::memory_order_relaxed);
    snap.wal_recovery.checkpoint_duration_us = checkpoint_duration_us_.load(std::memory_order_relaxed);
    snap.wal_recovery.recoveries_attempted = recoveries_attempted_.load(std::memory_order_relaxed);
    snap.wal_recovery.recoveries_succeeded = recoveries_succeeded_.load(std::memory_order_relaxed);
    snap.wal_recovery.recovery_duration_us = recovery_duration_us_.load(std::memory_order_relaxed);
    snap.wal_recovery.pages_redone = pages_redone_.load(std::memory_order_relaxed);
    snap.wal_recovery.pages_undone = pages_undone_.load(std::memory_order_relaxed);

    // Backup & Migration
    snap.backup_migration.backup_attempts = backup_attempts_.load(std::memory_order_relaxed);
    snap.backup_migration.backup_successes = backup_successes_.load(std::memory_order_relaxed);
    snap.backup_migration.backup_bytes = backup_bytes_.load(std::memory_order_relaxed);
    snap.backup_migration.backup_duration_us = backup_duration_us_.load(std::memory_order_relaxed);
    snap.backup_migration.verification_attempts = verification_attempts_.load(std::memory_order_relaxed);
    snap.backup_migration.verification_failures = verification_failures_.load(std::memory_order_relaxed);
    snap.backup_migration.restore_attempts = restore_attempts_.load(std::memory_order_relaxed);
    snap.backup_migration.restore_successes = restore_successes_.load(std::memory_order_relaxed);
    snap.backup_migration.restore_duration_us = restore_duration_us_.load(std::memory_order_relaxed);
    snap.backup_migration.sql_export_rows = sql_export_rows_.load(std::memory_order_relaxed);
    snap.backup_migration.sql_import_rows = sql_import_rows_.load(std::memory_order_relaxed);
    snap.backup_migration.csv_export_rows = csv_export_rows_.load(std::memory_order_relaxed);
    snap.backup_migration.csv_import_rows = csv_import_rows_.load(std::memory_order_relaxed);

    return snap;
}

void DatabaseMetrics::reset() noexcept {
    page_requests_.store(0, std::memory_order_relaxed);
    cache_hits_.store(0, std::memory_order_relaxed);
    cache_misses_.store(0, std::memory_order_relaxed);
    physical_reads_.store(0, std::memory_order_relaxed);
    physical_writes_.store(0, std::memory_order_relaxed);
    page_flushes_.store(0, std::memory_order_relaxed);
    evictions_.store(0, std::memory_order_relaxed);
    allocation_failures_.store(0, std::memory_order_relaxed);
    current_pinned_frames_.store(0, std::memory_order_relaxed);
    peak_pinned_frames_.store(0, std::memory_order_relaxed);

    statements_executed_.store(0, std::memory_order_relaxed);
    statements_successful_.store(0, std::memory_order_relaxed);
    statements_failed_.store(0, std::memory_order_relaxed);
    rows_examined_.store(0, std::memory_order_relaxed);
    rows_returned_.store(0, std::memory_order_relaxed);
    rows_inserted_.store(0, std::memory_order_relaxed);
    rows_updated_.store(0, std::memory_order_relaxed);
    rows_deleted_.store(0, std::memory_order_relaxed);
    table_scans_.store(0, std::memory_order_relaxed);
    index_seeks_.store(0, std::memory_order_relaxed);
    index_range_scans_.store(0, std::memory_order_relaxed);
    sort_operations_.store(0, std::memory_order_relaxed);
    execution_duration_us_.store(0, std::memory_order_relaxed);

    transactions_started_.store(0, std::memory_order_relaxed);
    transactions_committed_.store(0, std::memory_order_relaxed);
    transactions_aborted_.store(0, std::memory_order_relaxed);
    rollbacks_executed_.store(0, std::memory_order_relaxed);
    commit_failures_.store(0, std::memory_order_relaxed);
    lock_waits_.store(0, std::memory_order_relaxed);
    lock_timeouts_.store(0, std::memory_order_relaxed);
    lock_wait_duration_us_.store(0, std::memory_order_relaxed);

    wal_bytes_written_.store(0, std::memory_order_relaxed);
    wal_bytes_flushed_.store(0, std::memory_order_relaxed);
    wal_records_written_.store(0, std::memory_order_relaxed);
    wal_records_scanned_.store(0, std::memory_order_relaxed);
    checkpoints_attempted_.store(0, std::memory_order_relaxed);
    checkpoints_succeeded_.store(0, std::memory_order_relaxed);
    checkpoint_duration_us_.store(0, std::memory_order_relaxed);
    recoveries_attempted_.store(0, std::memory_order_relaxed);
    recoveries_succeeded_.store(0, std::memory_order_relaxed);
    recovery_duration_us_.store(0, std::memory_order_relaxed);
    pages_redone_.store(0, std::memory_order_relaxed);
    pages_undone_.store(0, std::memory_order_relaxed);

    backup_attempts_.store(0, std::memory_order_relaxed);
    backup_successes_.store(0, std::memory_order_relaxed);
    backup_bytes_.store(0, std::memory_order_relaxed);
    backup_duration_us_.store(0, std::memory_order_relaxed);
    verification_attempts_.store(0, std::memory_order_relaxed);
    verification_failures_.store(0, std::memory_order_relaxed);
    restore_attempts_.store(0, std::memory_order_relaxed);
    restore_successes_.store(0, std::memory_order_relaxed);
    restore_duration_us_.store(0, std::memory_order_relaxed);
    sql_export_rows_.store(0, std::memory_order_relaxed);
    sql_import_rows_.store(0, std::memory_order_relaxed);
    csv_export_rows_.store(0, std::memory_order_relaxed);
    csv_import_rows_.store(0, std::memory_order_relaxed);
}

std::string DatabaseMetricsSnapshot::to_string() const {
    std::ostringstream oss;
    oss << "======================================================================\n";
    oss << "                   KLYRDB DATABASE RUNTIME METRICS                    \n";
    oss << "======================================================================\n";
    oss << "Database: " << (database_name.empty() ? "(in-memory / unnamed)" : database_name) << "\n";
    oss << "----------------------------------------------------------------------\n";

    oss << "1. Buffer Pool & Cache Management:\n";
    oss << "  - Page Requests:           " << buffer_pool.page_requests << "\n";
    oss << "  - Cache Hits:              " << buffer_pool.cache_hits
        << " (" << std::fixed << std::setprecision(2) << (buffer_pool.hit_ratio() * 100.0) << "%)\n";
    oss << "  - Cache Misses:            " << buffer_pool.cache_misses << "\n";
    oss << "  - Physical Page Reads:     " << buffer_pool.physical_reads << "\n";
    oss << "  - Physical Page Writes:    " << buffer_pool.physical_writes << "\n";
    oss << "  - Dirty Page Flushes:      " << buffer_pool.page_flushes << "\n";
    oss << "  - Buffer Evictions:        " << buffer_pool.evictions << "\n";
    oss << "  - Allocation Failures:     " << buffer_pool.allocation_failures << "\n";
    oss << "  - Pinned Frames (Cur/Peak):" << buffer_pool.current_pinned_frames << " / "
        << buffer_pool.peak_pinned_frames << "\n\n";

    oss << "2. Query Execution:\n";
    oss << "  - Statements (Total/OK/Err):" << queries.statements_executed << " / "
        << queries.statements_successful << " / " << queries.statements_failed << "\n";
    oss << "  - Rows Examined / Returned: " << queries.rows_examined << " / " << queries.rows_returned << "\n";
    oss << "  - Rows (Ins/Upd/Del):       " << queries.rows_inserted << " / "
        << queries.rows_updated << " / " << queries.rows_deleted << "\n";
    oss << "  - Table Scans:              " << queries.table_scans << "\n";
    oss << "  - Index Seeks / Range Scans:" << queries.index_seeks << " / " << queries.index_range_scans << "\n";
    oss << "  - In-Memory Sort Operations:" << queries.sort_operations << "\n";
    double q_ms = static_cast<double>(queries.execution_duration_us) / 1000.0;
    oss << "  - Total Query Execution:    " << std::fixed << std::setprecision(2) << q_ms << " ms\n\n";

    oss << "3. Transactions & Concurrency:\n";
    oss << "  - Txns Started / Committed: " << transactions.transactions_started << " / "
        << transactions.transactions_committed << "\n";
    oss << "  - Txns Aborted / Rolled Back:" << transactions.transactions_aborted << " / "
        << transactions.rollbacks_executed << "\n";
    oss << "  - Commit Failures:          " << transactions.commit_failures << "\n";
    oss << "  - Lock Waits / Timeouts:    " << transactions.lock_waits << " / "
        << transactions.lock_timeouts << "\n";
    double lk_ms = static_cast<double>(transactions.lock_wait_duration_us) / 1000.0;
    oss << "  - Lock Wait Duration:       " << std::fixed << std::setprecision(2) << lk_ms << " ms\n\n";

    oss << "4. WAL, Checkpoints & Recovery:\n";
    oss << "  - WAL Bytes Written/Flushed:" << wal_recovery.wal_bytes_written << " / "
        << wal_recovery.wal_bytes_flushed << " bytes\n";
    oss << "  - Log Records Written/Scan: " << wal_recovery.wal_records_written << " / "
        << wal_recovery.wal_records_scanned << "\n";
    oss << "  - Checkpoints (Att/Succ):   " << wal_recovery.checkpoints_attempted << " / "
        << wal_recovery.checkpoints_succeeded << "\n";
    oss << "  - Recoveries (Att/Succ):    " << wal_recovery.recoveries_attempted << " / "
        << wal_recovery.recoveries_succeeded << "\n";
    oss << "  - Recovery Pages (Redo/Undo):" << wal_recovery.pages_redone << " / "
        << wal_recovery.pages_undone << "\n\n";

    oss << "5. Backup & Data Migration:\n";
    oss << "  - Backups (Att/Succ/Bytes): " << backup_migration.backup_attempts << " / "
        << backup_migration.backup_successes << " / " << backup_migration.backup_bytes << " bytes\n";
    oss << "  - Restores (Att/Succ):      " << backup_migration.restore_attempts << " / "
        << backup_migration.restore_successes << "\n";
    oss << "  - Verifications (Att/Fail): " << backup_migration.verification_attempts << " / "
        << backup_migration.verification_failures << "\n";
    oss << "  - SQL Migration (Exp/Imp):  " << backup_migration.sql_export_rows << " / "
        << backup_migration.sql_import_rows << " rows\n";
    oss << "  - CSV Migration (Exp/Imp):  " << backup_migration.csv_export_rows << " / "
        << backup_migration.csv_import_rows << " rows\n";
    oss << "======================================================================\n";
    return oss.str();
}

std::string DatabaseMetricsSnapshot::to_json() const {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"metrics_version\": 1,\n";
    oss << "  \"database_name\": \"" << database_name << "\",\n";
    oss << "  \"timestamp_utc_epoch_s\": " << timestamp_utc_epoch_s << ",\n";

    oss << "  \"buffer_pool\": {\n";
    oss << "    \"page_requests\": " << buffer_pool.page_requests << ",\n";
    oss << "    \"cache_hits\": " << buffer_pool.cache_hits << ",\n";
    oss << "    \"cache_misses\": " << buffer_pool.cache_misses << ",\n";
    oss << "    \"hit_ratio\": " << std::fixed << std::setprecision(4) << buffer_pool.hit_ratio() << ",\n";
    oss << "    \"physical_reads\": " << buffer_pool.physical_reads << ",\n";
    oss << "    \"physical_writes\": " << buffer_pool.physical_writes << ",\n";
    oss << "    \"page_flushes\": " << buffer_pool.page_flushes << ",\n";
    oss << "    \"evictions\": " << buffer_pool.evictions << ",\n";
    oss << "    \"allocation_failures\": " << buffer_pool.allocation_failures << ",\n";
    oss << "    \"current_pinned_frames\": " << buffer_pool.current_pinned_frames << ",\n";
    oss << "    \"peak_pinned_frames\": " << buffer_pool.peak_pinned_frames << "\n";
    oss << "  },\n";

    oss << "  \"queries\": {\n";
    oss << "    \"statements_executed\": " << queries.statements_executed << ",\n";
    oss << "    \"statements_successful\": " << queries.statements_successful << ",\n";
    oss << "    \"statements_failed\": " << queries.statements_failed << ",\n";
    oss << "    \"rows_examined\": " << queries.rows_examined << ",\n";
    oss << "    \"rows_returned\": " << queries.rows_returned << ",\n";
    oss << "    \"rows_inserted\": " << queries.rows_inserted << ",\n";
    oss << "    \"rows_updated\": " << queries.rows_updated << ",\n";
    oss << "    \"rows_deleted\": " << queries.rows_deleted << ",\n";
    oss << "    \"table_scans\": " << queries.table_scans << ",\n";
    oss << "    \"index_seeks\": " << queries.index_seeks << ",\n";
    oss << "    \"index_range_scans\": " << queries.index_range_scans << ",\n";
    oss << "    \"sort_operations\": " << queries.sort_operations << ",\n";
    oss << "    \"execution_duration_us\": " << queries.execution_duration_us << "\n";
    oss << "  },\n";

    oss << "  \"transactions\": {\n";
    oss << "    \"transactions_started\": " << transactions.transactions_started << ",\n";
    oss << "    \"transactions_committed\": " << transactions.transactions_committed << ",\n";
    oss << "    \"transactions_aborted\": " << transactions.transactions_aborted << ",\n";
    oss << "    \"rollbacks_executed\": " << transactions.rollbacks_executed << ",\n";
    oss << "    \"commit_failures\": " << transactions.commit_failures << ",\n";
    oss << "    \"lock_waits\": " << transactions.lock_waits << ",\n";
    oss << "    \"lock_timeouts\": " << transactions.lock_timeouts << ",\n";
    oss << "    \"lock_wait_duration_us\": " << transactions.lock_wait_duration_us << "\n";
    oss << "  },\n";

    oss << "  \"wal_recovery\": {\n";
    oss << "    \"wal_bytes_written\": " << wal_recovery.wal_bytes_written << ",\n";
    oss << "    \"wal_bytes_flushed\": " << wal_recovery.wal_bytes_flushed << ",\n";
    oss << "    \"wal_records_written\": " << wal_recovery.wal_records_written << ",\n";
    oss << "    \"wal_records_scanned\": " << wal_recovery.wal_records_scanned << ",\n";
    oss << "    \"checkpoints_attempted\": " << wal_recovery.checkpoints_attempted << ",\n";
    oss << "    \"checkpoints_succeeded\": " << wal_recovery.checkpoints_succeeded << ",\n";
    oss << "    \"checkpoint_duration_us\": " << wal_recovery.checkpoint_duration_us << ",\n";
    oss << "    \"recoveries_attempted\": " << wal_recovery.recoveries_attempted << ",\n";
    oss << "    \"recoveries_succeeded\": " << wal_recovery.recoveries_succeeded << ",\n";
    oss << "    \"recovery_duration_us\": " << wal_recovery.recovery_duration_us << ",\n";
    oss << "    \"pages_redone\": " << wal_recovery.pages_redone << ",\n";
    oss << "    \"pages_undone\": " << wal_recovery.pages_undone << "\n";
    oss << "  },\n";

    oss << "  \"backup_migration\": {\n";
    oss << "    \"backup_attempts\": " << backup_migration.backup_attempts << ",\n";
    oss << "    \"backup_successes\": " << backup_migration.backup_successes << ",\n";
    oss << "    \"backup_bytes\": " << backup_migration.backup_bytes << ",\n";
    oss << "    \"backup_duration_us\": " << backup_migration.backup_duration_us << ",\n";
    oss << "    \"verification_attempts\": " << backup_migration.verification_attempts << ",\n";
    oss << "    \"verification_failures\": " << backup_migration.verification_failures << ",\n";
    oss << "    \"restore_attempts\": " << backup_migration.restore_attempts << ",\n";
    oss << "    \"restore_successes\": " << backup_migration.restore_successes << ",\n";
    oss << "    \"restore_duration_us\": " << backup_migration.restore_duration_us << ",\n";
    oss << "    \"sql_export_rows\": " << backup_migration.sql_export_rows << ",\n";
    oss << "    \"sql_import_rows\": " << backup_migration.sql_import_rows << ",\n";
    oss << "    \"csv_export_rows\": " << backup_migration.csv_export_rows << ",\n";
    oss << "    \"csv_import_rows\": " << backup_migration.csv_import_rows << "\n";
    oss << "  }\n";
    oss << "}\n";
    return oss.str();
}

DatabaseMetricsRegistry& DatabaseMetricsRegistry::instance() noexcept {
    static DatabaseMetricsRegistry reg;
    return reg;
}

std::shared_ptr<DatabaseMetrics> DatabaseMetricsRegistry::get_metrics(const std::filesystem::path& db_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;
    std::string key = std::filesystem::absolute(db_path, ec).lexically_normal().string();

    auto it = registry_.find(key);
    if (it != registry_.end()) {
        return it->second;
    }

    auto m = std::make_shared<DatabaseMetrics>(db_path.filename().string());
    registry_[key] = m;
    return m;
}

void DatabaseMetricsRegistry::reset_metrics(const std::filesystem::path& db_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;
    std::string key = std::filesystem::absolute(db_path, ec).lexically_normal().string();

    auto it = registry_.find(key);
    if (it != registry_.end()) {
        it->second->reset();
    }
}

void DatabaseMetricsRegistry::clean_unused() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = registry_.begin(); it != registry_.end();) {
        if (it->second.use_count() == 1) {
            it = registry_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace klyrdb
