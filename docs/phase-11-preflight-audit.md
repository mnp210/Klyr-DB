# KlyrDB Phase 11 — Preflight Audit Report

**Date:** 2026-10-23  
**Environment:** Windows 10/11 x64, MSVC 19.51 (Visual Studio 2022/2026 BuildTools), CMake 3.20, C++20  
**Baseline Status:** 43/43 CTest suites passing (100% pass rate)

---

## 1. Baseline Test Verification

The full 43-suite CTest regression suite was executed directly against `build/Debug`:

```text
100% tests passed, 0 tests failed out of 43
Total Test time (real) = 58.88 sec
```

Every component across Phases 1–10 (page file, buffer pool, slotted pages, record manager, B+ tree, free-space map, catalog, parser, executor, WAL, transactions, crash recovery, constraints, query planner, locking, checkpointing, physical backup/restore, logical export/import, CSV migration) is functioning with zero regressions.

---

## 2. Existing Telemetry & Counter Inventory

An inspection of the codebase identified the following fragmented state and operational measurements:

| Subsystem | Existing In-Memory State / Counters | Missing Observability Elements |
|---|---|---|
| **Buffer Pool** | `pool_size_`, `free_list_.size()`, `pinned_frame_count()`, `is_dirty(page_id)` | Cache hits, cache misses, physical page reads, physical page writes, dirty frame flushes, evictions, allocation failures. |
| **SQL Execution** | `QueryResult::rows_affected()`, `row_count()` | Total statements executed, success/failure rate, total rows examined, rows returned/mutated, full table scans vs index seeks/range scans, sort operations, query latency. |
| **Query Planner** | `PlanCost` (`estimated_cost`, `estimated_rows`, `estimated_pages`), `ExecutionMetrics` (`rows_examined`, `rows_produced`, `elapsed_time_ns`) | Dynamic profiling comparing estimates against actuals (`EXPLAIN ANALYZE`), table statistics caching (currently iterates all pages on every plan). |
| **Transactions** | `active_txns_.size()`, `last_txn_id_` | Total transactions started, committed, aborted, rollbacks executed, lock wait duration, lock contention timeouts. |
| **WAL & Recovery** | `last_lsn_`, `flushed_lsn_`, `file_size()`, `CheckpointStats`, `RecoveryStats` | Cumulative WAL bytes written/flushed, log records generated/scanned, checkpoint frequencies, cumulative recovery durations. |
| **Backup & Migration** | `BackupResult`, `VerificationReport` (`checks_passed`, `failures`), `CsvResult` (`rows_processed`) | Cumulative backup attempts/successes, backup bytes/duration, verification failures, restore attempts/successes, migration throughput. |

---

## 3. Concurrency, Overhead & Safety Analysis

1. **Thread Safety:**
   - Database operations in KlyrDB are multi-threaded (e.g. concurrent readers in `test_concurrent_reads.cpp`).
   - Metrics counters must use `std::atomic<uint64_t>` or lock-free memory orderings (`std::memory_order_relaxed` for increments, `std::memory_order_acquire`/`release` for snapshots/resets) to prevent cache-line contention and lock ordering deadlocks.
2. **Lock Independence:**
   - Diagnostic output formatting (`klyrdb stats`, `klyrdb diagnostics`) must NEVER hold long-lived exclusive database locks.
   - Read-only inspections must acquire brief shared locks or non-blocking locks (`try_lock`), gracefully reporting `"DATABASE_BUSY"` when writers hold active exclusive locks.
3. **Sensitive Data Privacy:**
   - In accordance with non-negotiable Rule 5, diagnostic logs, profiling output, and metrics dumps must NEVER expose raw SQL parameter literals, credentials, or record payloads. Only structural schema names, row counts, and timing telemetry are exposed.
4. **On-Disk Compatibility:**
   - On-disk Page 0 metadata and table catalog formats must be preserved. Any new table/index statistics will be maintained safely in dedicated diagnostic catalog pages or computed on-demand with versioned headers.

---

## 4. Phase 11 Staged Implementation Plan

- **Stage 1 — Metrics Infrastructure:**
  - Create `include/klyrdb/diagnostics/metrics.hpp` and `src/diagnostics/metrics.cpp`.
  - Provide strongly-typed `BufferPoolMetrics`, `QueryMetrics`, `TransactionMetrics`, `WalMetrics`, and `BackupMetrics`.
  - Wire counters into `BufferPoolManager`, `SqlExecutor`, `TransactionManager`, `WriteAheadLog`, `BackupManager`, and `RestoreManager`.
  - Add unit test suite `tests/test_metrics.cpp`.
- **Stage 2 — Runtime Diagnostics CLI:**
  - Implement `klyrdb stats <db>`, `klyrdb stats-reset <db>`, `klyrdb diagnostics <db> [--json]`.
  - Support human-readable text and strict JSON formats.
- **Stage 3 — Query Profiling (`EXPLAIN ANALYZE`):**
  - Extend SQL parser and AST for `EXPLAIN ANALYZE SELECT ...`.
  - Execute read-only plans with detailed timing and execution tracking: chosen access path, estimated vs actual rows, rows examined, index seeks/scans, explicit sort vs index-order, LIMIT pushdown, planning and execution durations.
  - Add test suite `tests/test_query_profiling.cpp`.
- **Stage 4 — Table and Index Statistics (`ANALYZE`):**
  - Implement `TableStatistics` engine and `klyrdb analyze <db>` / `ANALYZE [table];` command.
  - Gather exact row counts, page counts, distinct key estimates, null counts, and timestamps.
  - Integrate with `QueryPlanner` to replace the unindexed full-table scan in `estimate_table_rows()`.
- **Stage 5 — Reproducible Performance Benchmarks:**
  - Extend benchmark suite with increasing dataset sizes (1k, 10k, 100k rows where practical).
  - Add `benchmarks/benchmark_reliability.cpp` and update `CMakeLists.txt`.
- **Stage 6 — Property-Based and Randomized SQL Testing:**
  - Implement `tests/test_randomized_sql.cpp` using an independent in-memory reference model (ground truth).
  - Random sequences of DDL, INSERT, SELECT, UPDATE, DELETE, BEGIN, COMMIT, ROLLBACK with fixed repeatable seeds.
- **Stage 7 — Parser and Binary-Format Fuzzing / Mutation Testing:**
  - Implement `tests/test_fuzz_mutations.cpp` covering SQL lexer/parser, SQL dump reader, CSV parser, manifest JSON decoder, WAL record parser, page/row codecs, and B+ tree key decoders.
- **Stage 8 — Long-Running Reliability and Crash Stress Tests:**
  - Implement `tests/test_reliability_stress.cpp` combining concurrent readers, serialized writers, random rollback, checkpointing, and safe recovery verification.
- **Stage 9 — Error Handling & Diagnostics Quality Audit:**
  - Audit error messages across storage, parser, executor, WAL, and backup subsystems.
- **Stage 10 — Full Regression Validation:**
  - Build clean and verify 100% pass rate across all 43 prior suites + all new Phase 11 suites.
- **Stage 11 & 12 — Documentation & Delivery Report:**
  - Complete all documentation files and delivery report.
