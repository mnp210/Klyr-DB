# Phase 13 Adversarial Audit & Systems Review

**Date:** 2026-10-24  
**Project:** KlyrDB C++20 Database Engine  
**Version:** 0.12.0 Release Candidate  
**Review Scope:** Adversarial Edge Testing, Recovery Hardening, Packaging & Release Quality  

---

## 1. Executive Summary & Audit Methodology

During Phase 13, KlyrDB underwent an evidence-driven adversarial systems audit. Instead of adding speculative features, the codebase was inspected and stressed against failure scenarios including corrupted headers, torn tail writes, truncated payloads, multiple consecutive crash restarts, malicious buffer lengths, and concurrent locking timeouts.

The audit verified engine invariants, identified subtle recovery edge cases, implemented corrective patches, and added a dedicated adversarial test suite (`test_adversarial.cpp`). All 51 CTest test suites now pass with 100% reliability.

---

## 2. Key Discoveries and Engineering Fixes

### 2.1 ARIES Crash Recovery: Durable Abort Records for Undone Transactions

- **Defect Identified:** In prior phases, `RecoveryManager::recover()` identified uncommitted active transactions during the analysis pass, performed undo operations on modified pages during the undo pass, and flushed the pages. However, it did not append an `Abort` log record for those undone transactions. Consequently, if recovery was invoked a second time on the same log (e.g. consecutive crashes or restarts without intermediate activity), the uncommitted transactions remained listed as uncommitted in the log analysis, causing redundant undo evaluations.
- **Remediation:** `RecoveryManager::recover()` now iterates over all transactions in `txns_to_undo` upon completing the undo pass and explicitly appends a durable `LogRecord::make_abort(INVALID_LSN, tid, ...)` to the Write-Ahead Log. Subsequent recovery passes immediately recognize that the transaction is fully aborted, resulting in 0 pages redone, 0 pages undone, and true idempotence.
- **Verification:** Verified via `test_recovery_adversarial_idempotence` across 3 consecutive invocations of `RecoveryManager::recover()`.

### 2.2 Page 0 Metadata Checkpoint LSN Synchronization

- **Defect Identified:** After completing crash recovery, `RecoveryManager` appended a Checkpoint log record and updated the WAL file header's checkpoint boundary, but did not update the persistent `checkpoint_lsn` field in the database file's Page 0 (`DatabaseMetadata`). This created a potential skew between the WAL header and Page 0.
- **Remediation:** Added explicit metadata synchronization in `RecoveryManager::recover()` to encode `meta.checkpoint_lsn` into Page 0 and durably flush and sync it to disk alongside the WAL checkpoint boundary update.

### 2.3 SQL Standard Semantics: Multiple NULLs in UNIQUE Constraints

- **Audit Finding:** Checked unique constraint enforcement in `SqlExecutor::validate_insert_constraints` and `validate_update_constraints`.
- **Behavior Verified:** According to ISO SQL standards and relational integrity semantics, multiple rows with `NULL` in a `UNIQUE` column do not conflict with each other (unlike duplicate non-null values). The executor explicitly tests `if (val.is_null()) continue;` for UNIQUE indexes, while strictly enforcing `PRIMARY KEY` non-null constraints.
- **Verification:** Exercised in `test_sql_adversarial_null_and_unique`.

---

## 3. Subsystem Audit Matrix

| Subsystem | Audit Scenario Tested | System Invariant Verified | Outcome |
|---|---|---|---|
| **WAL LogRecord** | Buffer smaller than 48-byte header | Returns `StatusCode::TruncatedFile` | PASS |
| **WAL LogRecord** | Invalid magic signature (`!WALR`) | Returns `StatusCode::InvalidMagic` | PASS |
| **WAL LogRecord** | Image lengths > 4096 bytes | Returns `StatusCode::WalCorrupted` | PASS |
| **WAL LogRecord** | Bit flip in CRC-32 checksum | Returns `StatusCode::ChecksumMismatch` | PASS |
| **WAL File Header** | Truncated file header (< 32 bytes) | Returns `StatusCode::TruncatedFile` | PASS |
| **WAL File Header** | Corrupted page size in WAL header | Returns `StatusCode::PageCorrupted` | PASS |
| **WAL Crash Tail** | Torn write mid-payload | Trailing partial write safely skipped | PASS |
| **Page Header** | Payload size > 4072 bytes | Returns `StatusCode::PayloadOverflow` | PASS |
| **Page Header** | Unknown page type code | Returns `StatusCode::PageCorrupted` | PASS |
| **Concurrency** | Reentrant exclusive lock by same thread | Increments recursion count, no deadlock | PASS |
| **Concurrency** | Lock timeout on conflicting write | Returns `StatusCode::LockTimeout` | PASS |
| **Atomic Restore** | Staged restore with process crash guard | Uncommitted staging files automatically deleted | PASS |
| **CSV Migrator** | Escaped quotes, newlines, and `\N` literals | Clean RFC 4180 parsing without data corruption | PASS |

---

## 4. Benchmark Reproducibility

Reproducible benchmarks measured on Windows 11 x64 (MSVC 19.51, Debug configuration):
- **Sequential Inserts (1,000 ops):** 968.1 ops/sec (P50: 916.3 μs, P95: 1641.3 μs, P99: 2525.8 μs, Cache Hit: 99.41%)
- **Point Seeks (1,000 ops):** 1357.3 ops/sec (P50: 653.4 μs, P95: 1121.0 μs, P99: 1340.5 μs, Cache Hit: 99.92%)
- **Index Range Scans (500 ops):** 674.8 ops/sec (P50: 1377.8 μs, P95: 2073.7 μs, P99: 2487.7 μs, Cache Hit: 99.95%)
- **Point Updates (1,000 ops):** 823.5 ops/sec (P50: 1109.2 μs, P95: 2044.9 μs, P99: 2585.5 μs, Cache Hit: 99.95%)
- **Full Table Statistics Generation (`ANALYZE`):** 15.27 ms elapsed time.
