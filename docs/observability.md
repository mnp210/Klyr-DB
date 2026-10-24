# KlyrDB Observability & Runtime Metrics Architecture

## 1. Overview

Phase 11 introduces a production-grade, thread-safe diagnostics and observability subsystem to KlyrDB. Operating entirely in-memory with zero disk overhead on critical query paths, the observability subsystem aggregates real-time metrics across all database components:
- **Buffer Pool Manager**: Page requests, cache hits/misses, hit ratio, physical I/O, evictions, allocation failures, and pinned frame tracking.
- **SQL Execution Engine**: Statement counts (total, successful, failed), latency tracking, row metrics (examined, returned, inserted, updated, deleted), scan methods (table scan vs index seek/range scan), and in-memory sorts.
- **Transactions & Concurrency**: Txn lifecycle (started, committed, aborted, rolled back), commit failures, lock waits, timeouts, and contention duration.
- **Write-Ahead Logging & Recovery**: WAL bytes written/flushed, log records generated/scanned, checkpoint attempts/successes, recovery invocations, and pages redone/undone.
- **Backup & Disaster Recovery**: Physical backup attempts/successes/bytes, restore operations, integrity verifications, and logical/CSV export/import row counts.

---

## 2. Architecture & Concurrency Model

### Thread-Safe Atomic Accumulators
All metrics within `DatabaseMetrics` are maintained using `std::atomic<uint64_t>` accumulators with `std::memory_order_relaxed` for low overhead under multi-threaded concurrency. Peak gauges (such as peak pinned frames) use compare-and-swap exchange loops (`compare_exchange_weak`).

```cpp
class DatabaseMetrics {
public:
    void record_cache_hit();
    void record_cache_miss();
    void record_physical_read();
    void record_physical_write();
    void record_page_flush();
    void record_eviction();
    void record_statement_executed(bool success, uint64_t duration_us);
    void record_rows_examined(uint64_t count);
    void record_table_scan();
    void record_index_seek();
    ...
    DatabaseMetricsSnapshot snapshot() const;
    void reset();
};
```

### Global Metrics Registry
`DatabaseMetricsRegistry` provides a process-wide singleton managing per-database metrics instances:
- Thread-safe registry access with `std::mutex`.
- Auto-registers or retrieves metrics instances by database path/name.
- Enables CLI tools (`klyrdb stats`, `klyrdb diagnostics`) to inspect runtime activity without interfering with active database handles.

---

## 3. Snapshot Data Structures & Formats

A call to `DatabaseMetrics::snapshot()` produces a point-in-time immutable `DatabaseMetricsSnapshot` struct with built-in export formatters:
- `to_string()`: Human-readable diagnostic summary grouped into 5 clear subsystem categories.
- `to_json()`: RFC 8259 compliant structured JSON for monitoring agents, dashboards, and automated verification suites.

### JSON Output Specification
```json
{
  "metrics_version": 1,
  "database_name": "production.kdb",
  "timestamp_utc_epoch_s": 1792759393,
  "buffer_pool": {
    "page_requests": 175418,
    "cache_hits": 175324,
    "cache_misses": 94,
    "hit_ratio": 0.9995,
    "physical_reads": 94,
    "physical_writes": 94,
    "page_flushes": 0,
    "evictions": 0,
    "allocation_failures": 0,
    "current_pinned_frames": 0,
    "peak_pinned_frames": 4
  },
  "queries": {
    "statements_executed": 3503,
    "statements_successful": 3503,
    "statements_failed": 0,
    "rows_examined": 21100,
    "rows_returned": 21100,
    "rows_inserted": 1000,
    "rows_updated": 1000,
    "rows_deleted": 0,
    "table_scans": 0,
    "index_seeks": 1000,
    "index_range_scans": 500,
    "sort_operations": 0,
    "execution_duration_us": 3373930
  },
  "transactions": {
    "transactions_started": 2000,
    "transactions_committed": 2000,
    "transactions_aborted": 0,
    "rollbacks_executed": 0,
    "commit_failures": 0,
    "lock_waits": 0,
    "lock_timeouts": 0,
    "lock_wait_duration_us": 0
  },
  "wal_recovery": { ... },
  "backup_migration": { ... }
}
```

---

## 4. CLI Runtime Diagnostics Tools

KlyrDB provides four dedicated CLI diagnostic commands:

1. **`klyrdb stats <db_file>`**
   Renders human-readable metrics table for the specified database file.

2. **`klyrdb stats-reset <db_file>`**
   Resets runtime metric counters and cached catalog statistics.

3. **`klyrdb diagnostics <db_file> [--json]`**
   Executes a non-destructive storage integrity check (`IntegrityChecker::check_all()`), pulls runtime metrics, inspects table/index statistics, and formats a combined diagnostic health report.

4. **`klyrdb analyze <db_file> [table_name]`**
   Collects distribution statistics across all or specific tables, calculating exact row counts, page footprints, null counts, distinct values, and min/max key bounds. Caches statistics in memory for the query planner.
