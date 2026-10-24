#include "test_framework.hpp"
#include "klyrdb/diagnostics/metrics.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/sql/executor.hpp"

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_metrics_defaults_and_snapshot() {
    DatabaseMetrics metrics("test_db");
    auto snap = metrics.snapshot();

    EXPECT_EQ(snap.database_name, "test_db");
    EXPECT_EQ(snap.buffer_pool.page_requests, 0ULL);
    EXPECT_EQ(snap.buffer_pool.cache_hits, 0ULL);
    EXPECT_EQ(snap.buffer_pool.cache_misses, 0ULL);
    EXPECT_EQ(snap.buffer_pool.hit_ratio(), 0.0);
    EXPECT_EQ(snap.queries.statements_executed, 0ULL);
    EXPECT_EQ(snap.transactions.transactions_started, 0ULL);
    EXPECT_EQ(snap.wal_recovery.wal_bytes_written, 0ULL);

    std::string str = snap.to_string();
    EXPECT_TRUE(str.find("test_db") != std::string::npos);
    EXPECT_TRUE(str.find("Buffer Pool & Cache Management") != std::string::npos);

    std::string json = snap.to_json();
    EXPECT_TRUE(json.find("\"database_name\": \"test_db\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"buffer_pool\": {") != std::string::npos);
}

static void test_buffer_pool_metrics_recording() {
    DatabaseMetrics metrics("test_bp");

    metrics.record_page_request();
    metrics.record_cache_hit();
    metrics.record_page_request();
    metrics.record_cache_miss();
    metrics.record_physical_read();
    metrics.record_physical_write();
    metrics.record_page_flush();
    metrics.record_eviction();
    metrics.record_allocation_failure();
    metrics.update_pinned_frames(5);
    metrics.update_pinned_frames(12);
    metrics.update_pinned_frames(3);

    auto snap = metrics.snapshot();
    EXPECT_EQ(snap.buffer_pool.page_requests, 2ULL);
    EXPECT_EQ(snap.buffer_pool.cache_hits, 1ULL);
    EXPECT_EQ(snap.buffer_pool.cache_misses, 1ULL);
    EXPECT_EQ(snap.buffer_pool.hit_ratio(), 0.5);
    EXPECT_EQ(snap.buffer_pool.physical_reads, 1ULL);
    EXPECT_EQ(snap.buffer_pool.physical_writes, 1ULL);
    EXPECT_EQ(snap.buffer_pool.page_flushes, 1ULL);
    EXPECT_EQ(snap.buffer_pool.evictions, 1ULL);
    EXPECT_EQ(snap.buffer_pool.allocation_failures, 1ULL);
    EXPECT_EQ(snap.buffer_pool.current_pinned_frames, 3ULL);
    EXPECT_EQ(snap.buffer_pool.peak_pinned_frames, 12ULL);

    metrics.reset();
    auto reset_snap = metrics.snapshot();
    EXPECT_EQ(reset_snap.buffer_pool.page_requests, 0ULL);
    EXPECT_EQ(reset_snap.buffer_pool.peak_pinned_frames, 0ULL);
}

static void test_query_and_txn_metrics_recording() {
    DatabaseMetrics metrics("test_query_txn");

    metrics.record_statement_executed(true, 1500);
    metrics.record_statement_executed(false, 300);
    metrics.record_rows_examined(50);
    metrics.record_rows_returned(10);
    metrics.record_rows_inserted(5);
    metrics.record_rows_updated(3);
    metrics.record_rows_deleted(2);
    metrics.record_table_scan();
    metrics.record_index_seek();
    metrics.record_index_range_scan();
    metrics.record_sort_operation();

    metrics.record_transaction_started();
    metrics.record_transaction_committed();
    metrics.record_transaction_started();
    metrics.record_transaction_aborted();
    metrics.record_rollback();
    metrics.record_commit_failure();
    metrics.record_lock_wait(500);
    metrics.record_lock_timeout();

    auto snap = metrics.snapshot();
    EXPECT_EQ(snap.queries.statements_executed, 2ULL);
    EXPECT_EQ(snap.queries.statements_successful, 1ULL);
    EXPECT_EQ(snap.queries.statements_failed, 1ULL);
    EXPECT_EQ(snap.queries.rows_examined, 50ULL);
    EXPECT_EQ(snap.queries.rows_returned, 10ULL);
    EXPECT_EQ(snap.queries.rows_inserted, 5ULL);
    EXPECT_EQ(snap.queries.rows_updated, 3ULL);
    EXPECT_EQ(snap.queries.rows_deleted, 2ULL);
    EXPECT_EQ(snap.queries.table_scans, 1ULL);
    EXPECT_EQ(snap.queries.index_seeks, 1ULL);
    EXPECT_EQ(snap.queries.index_range_scans, 1ULL);
    EXPECT_EQ(snap.queries.sort_operations, 1ULL);
    EXPECT_EQ(snap.queries.execution_duration_us, 1800ULL);

    EXPECT_EQ(snap.transactions.transactions_started, 2ULL);
    EXPECT_EQ(snap.transactions.transactions_committed, 1ULL);
    EXPECT_EQ(snap.transactions.transactions_aborted, 1ULL);
    EXPECT_EQ(snap.transactions.rollbacks_executed, 1ULL);
    EXPECT_EQ(snap.transactions.commit_failures, 1ULL);
    EXPECT_EQ(snap.transactions.lock_waits, 1ULL);
    EXPECT_EQ(snap.transactions.lock_timeouts, 1ULL);
    EXPECT_EQ(snap.transactions.lock_wait_duration_us, 500ULL);
}

static void test_wal_and_backup_metrics_recording() {
    DatabaseMetrics metrics("test_wal_backup");

    metrics.record_wal_write(128);
    metrics.record_wal_write(256);
    metrics.record_wal_flush(384);
    metrics.record_wal_records_scanned(15);
    metrics.record_checkpoint(true, 4200);
    metrics.record_recovery(true, 8500, 10, 2);

    metrics.record_backup(true, 1048576, 12000);
    metrics.record_verification(true);
    metrics.record_restore(true, 9500);
    metrics.record_sql_export(100);
    metrics.record_sql_import(100);
    metrics.record_csv_export(200);
    metrics.record_csv_import(200);

    auto snap = metrics.snapshot();
    EXPECT_EQ(snap.wal_recovery.wal_bytes_written, 384ULL);
    EXPECT_EQ(snap.wal_recovery.wal_bytes_flushed, 384ULL);
    EXPECT_EQ(snap.wal_recovery.wal_records_scanned, 15ULL);
    EXPECT_EQ(snap.wal_recovery.checkpoints_attempted, 1ULL);
    EXPECT_EQ(snap.wal_recovery.checkpoints_succeeded, 1ULL);
    EXPECT_EQ(snap.wal_recovery.checkpoint_duration_us, 4200ULL);
    EXPECT_EQ(snap.wal_recovery.recoveries_attempted, 1ULL);
    EXPECT_EQ(snap.wal_recovery.recoveries_succeeded, 1ULL);
    EXPECT_EQ(snap.wal_recovery.recovery_duration_us, 8500ULL);
    EXPECT_EQ(snap.wal_recovery.pages_redone, 10ULL);
    EXPECT_EQ(snap.wal_recovery.pages_undone, 2ULL);

    EXPECT_EQ(snap.backup_migration.backup_attempts, 1ULL);
    EXPECT_EQ(snap.backup_migration.backup_successes, 1ULL);
    EXPECT_EQ(snap.backup_migration.backup_bytes, 1048576ULL);
    EXPECT_EQ(snap.backup_migration.verification_attempts, 1ULL);
    EXPECT_EQ(snap.backup_migration.restore_attempts, 1ULL);
    EXPECT_EQ(snap.backup_migration.restore_successes, 1ULL);
    EXPECT_EQ(snap.backup_migration.sql_export_rows, 100ULL);
    EXPECT_EQ(snap.backup_migration.sql_import_rows, 100ULL);
    EXPECT_EQ(snap.backup_migration.csv_export_rows, 200ULL);
    EXPECT_EQ(snap.backup_migration.csv_import_rows, 200ULL);
}

static void test_database_metrics_registry() {
    auto& reg = DatabaseMetricsRegistry::instance();
    auto m1 = reg.get_metrics("test_db1.klyr");
    auto m2 = reg.get_metrics("test_db1.klyr");
    EXPECT_EQ(m1.get(), m2.get());

    m1->record_cache_hit();
    EXPECT_EQ(m2->snapshot().buffer_pool.cache_hits, 1ULL);

    reg.reset_metrics("test_db1.klyr");
    EXPECT_EQ(m2->snapshot().buffer_pool.cache_hits, 0ULL);
}

static void test_end_to_end_sql_metrics_integration() {
    auto path = temp_test_file("test_metrics_e2e");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);

    auto pf = PageFile::create(path, false).value();
    auto wal = WriteAheadLog::open_or_create(wal_path).value();
    BufferPoolManager bpm(*pf, 32);

    auto metrics = DatabaseMetricsRegistry::instance().get_metrics(path);
    metrics->reset();
    bpm.set_metrics(metrics.get());
    bpm.set_wal(wal.get());

    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();
    TransactionManager tm(bpm, *wal);

    SqlExecutor executor(bpm, rm, *tc, *ic);
    executor.set_transaction_manager(&tm);

    EXPECT_TRUE(executor.execute("CREATE TABLE products (id INTEGER PRIMARY KEY, name TEXT, price INTEGER);").is_ok());
    EXPECT_TRUE(executor.execute("CREATE INDEX idx_prod_price ON products(price);").is_ok());

    EXPECT_TRUE(executor.execute("INSERT INTO products VALUES (1, 'Widget', 100);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO products VALUES (2, 'Gadget', 200);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO products VALUES (3, 'Doohickey', 300);").is_ok());

    auto snap1 = metrics->snapshot();
    EXPECT_TRUE(snap1.queries.statements_executed >= 5ULL);
    EXPECT_EQ(snap1.queries.rows_inserted, 3ULL);
    EXPECT_TRUE(snap1.buffer_pool.page_requests > 0ULL);

    // Run table scan query
    auto q1 = executor.execute("SELECT * FROM products;");
    EXPECT_TRUE(q1.is_ok());
    EXPECT_EQ(q1.value().rows().size(), 3ULL);

    // Run index seek query
    auto q2 = executor.execute("SELECT * FROM products WHERE id = 2;");
    EXPECT_TRUE(q2.is_ok());
    EXPECT_EQ(q2.value().rows().size(), 1ULL);

    // Run index range query
    auto q3 = executor.execute("SELECT * FROM products WHERE price > 150;");
    EXPECT_TRUE(q3.is_ok());
    EXPECT_EQ(q3.value().rows().size(), 2ULL);

    // Run update and delete
    EXPECT_TRUE(executor.execute("UPDATE products SET price = 150 WHERE id = 1;").is_ok());
    EXPECT_TRUE(executor.execute("DELETE FROM products WHERE id = 3;").is_ok());

    auto snap2 = metrics->snapshot();
    EXPECT_TRUE(snap2.queries.table_scans >= 1ULL);
    EXPECT_TRUE(snap2.queries.index_seeks >= 1ULL);
    EXPECT_TRUE(snap2.queries.index_range_scans >= 1ULL);
    EXPECT_EQ(snap2.queries.rows_updated, 1ULL);
    EXPECT_EQ(snap2.queries.rows_deleted, 1ULL);
    EXPECT_TRUE(snap2.transactions.transactions_started >= 5ULL);
    EXPECT_TRUE(snap2.transactions.transactions_committed >= 5ULL);
}

int main() {
    std::cout << "=== Running Diagnostics & Metrics Tests ===\n";

    run_test("test_metrics_defaults_and_snapshot", test_metrics_defaults_and_snapshot);
    run_test("test_buffer_pool_metrics_recording", test_buffer_pool_metrics_recording);
    run_test("test_query_and_txn_metrics_recording", test_query_and_txn_metrics_recording);
    run_test("test_wal_and_backup_metrics_recording", test_wal_and_backup_metrics_recording);
    run_test("test_database_metrics_registry", test_database_metrics_registry);
    run_test("test_end_to_end_sql_metrics_integration", test_end_to_end_sql_metrics_integration);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
