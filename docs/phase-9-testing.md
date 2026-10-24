# KlyrDB — Phase 9 Testing & Benchmark Report

## 1. Test Suite Summary

Phase 9 adds 6 dedicated automated test suites, expanding test coverage from 29 baseline test suites to 35 test suites.

### Complete CTest Execution Results:
```text
Test project D:/KlyrDB/build
      Start  1: test_page
 1/35 Test  #1: test_page ........................   Passed    0.10 sec
      Start  2: test_page_file
 2/35 Test  #2: test_page_file ...................   Passed    0.10 sec
      Start  3: test_persistence
 3/35 Test  #3: test_persistence .................   Passed    0.15 sec
      Start  4: test_buffer_pool
 4/35 Test  #4: test_buffer_pool .................   Passed    0.19 sec
      Start  5: test_slotted_page
 5/35 Test  #5: test_slotted_page ................   Passed    0.06 sec
      Start  6: test_record_manager
 6/35 Test  #6: test_record_manager ..............   Passed    0.14 sec
      Start  7: test_bplus_tree
 7/35 Test  #7: test_bplus_tree ..................   Passed    0.20 sec
      Start  8: test_fsm
 8/35 Test  #8: test_fsm .........................   Passed    0.10 sec
      Start  9: test_index_integration
 9/35 Test  #9: test_index_integration ...........   Passed    0.14 sec
      Start 10: test_sql_lexer
10/35 Test #10: test_sql_lexer ...................   Passed    0.04 sec
      Start 11: test_sql_parser
11/35 Test #11: test_sql_parser ..................   Passed    0.05 sec
      Start 12: test_table_catalog
12/35 Test #12: test_table_catalog ...............   Passed    0.09 sec
      Start 13: test_sql_executor
13/35 Test #13: test_sql_executor ................   Passed    0.15 sec
      Start 14: test_wal
14/35 Test #14: test_wal .........................   Passed    0.13 sec
      Start 15: test_transaction
15/35 Test #15: test_transaction .................   Passed    0.18 sec
      Start 16: test_recovery
16/35 Test #16: test_recovery ....................   Passed    0.34 sec
      Start 17: test_crash_recovery
17/35 Test #17: test_crash_recovery ..............   Passed    0.62 sec
      Start 18: test_constraints
18/35 Test #18: test_constraints .................   Passed    0.18 sec
      Start 19: test_constraint_transactions
19/35 Test #19: test_constraint_transactions .....   Passed    0.12 sec
      Start 20: test_query_planner
20/35 Test #20: test_query_planner ...............   Passed    0.08 sec
      Start 21: test_explain
21/35 Test #21: test_explain .....................   Passed    0.09 sec
      Start 22: test_query_plan_execution
22/35 Test #22: test_query_plan_execution ........   Passed    0.11 sec
      Start 23: test_query_optimization
23/35 Test #23: test_query_optimization ..........   Passed    0.11 sec
      Start 24: test_database_locking
24/35 Test #24: test_database_locking ............   Passed    5.45 sec
      Start 25: test_process_locking
25/35 Test #25: test_process_locking .............   Passed    0.21 sec
      Start 26: test_buffer_pool_concurrency
26/35 Test #26: test_buffer_pool_concurrency .....   Passed    0.19 sec
      Start 27: test_concurrent_reads
27/35 Test #27: test_concurrent_reads ............   Passed    0.29 sec
      Start 28: test_concurrent_writes
28/35 Test #28: test_concurrent_writes ...........   Passed    0.14 sec
      Start 29: test_transaction_isolation
29/35 Test #29: test_transaction_isolation .......   Passed    0.25 sec
      Start 30: test_checkpoint
30/35 Test #30: test_checkpoint ..................   Passed    0.22 sec
      Start 31: test_wal_retention
31/35 Test #31: test_wal_retention ...............   Passed    0.16 sec
      Start 32: test_checkpoint_recovery
32/35 Test #32: test_checkpoint_recovery .........   Passed    0.19 sec
      Start 33: test_integrity_check
33/35 Test #33: test_integrity_check .............   Passed    0.09 sec
      Start 34: test_storage_corruption
34/35 Test #34: test_storage_corruption ..........   Passed    0.13 sec
      Start 35: test_wal_lifecycle
35/35 Test #35: test_wal_lifecycle ...............   Passed    0.24 sec

100% tests passed, 0 tests failed out of 35
Total Test time (real) = 12.28 sec
```

---

## 2. Phase 9 Test Suites Detail

### 1. `test_checkpoint` (`tests/test_checkpoint.cpp`)
- **`test_checkpoint_record_encoding`**: Encodes and decodes checkpoint records containing active transaction lists and min-required LSNs, validating byte-level fidelity.
- **`test_checkpoint_flushes_dirty_pages`**: Validates that executing a checkpoint flushes all dirty pages to the database file, clearing dirty states in buffer pool frames.
- **`test_checkpoint_metadata_lsn_updated`**: Verifies that the persistent checkpoint LSN at offset 40 of Page 0 is updated and matches the WAL checkpoint record.

### 2. `test_wal_retention` (`tests/test_wal_retention.cpp`)
- **`test_wal_retention_all_committed`**: When all transactions are committed, `min_required_lsn` equals the checkpoint LSN, allowing safe truncation of all prior records.
- **`test_wal_retention_active_transaction`**: Verifies that when an active transaction is in-flight, `min_required_lsn` is clamped to the first uncommitted record's LSN, preventing premature truncation.
- **`test_wal_safe_truncation_atomic`**: Validates atomic replacement during WAL truncation (`.wal.tmp` swapped into `.wal`), ensuring no data loss if interrupted.

### 3. `test_checkpoint_recovery` (`tests/test_checkpoint_recovery.cpp`)
- **`test_recovery_from_checkpoint_boundary`**: Confirms that crash recovery begins scanning the WAL from `min_required_lsn` rather than LSN 1, skipping already persisted committed changes.
- **`test_recovery_ignores_flushed_pages`**: Asserts that redo passes check `page.page_lsn() >= record.lsn` and avoid redundant writes.
- **`test_recovery_undo_active_across_checkpoint`**: Injects an interrupted transaction across a checkpoint boundary and verifies that the undo phase cleanly rolls back uncommitted records.

### 4. `test_integrity_check` (`tests/test_integrity_check.cpp`)
- **`test_integrity_clean_database`**: Audits a clean database with multiple tables, indexes, records, and FSM entries, asserting 0 errors and 0 warnings.
- **`test_integrity_foreign_key_validation`**: Validates that orphan foreign keys are accurately detected and reported by the integrity engine.
- **`test_integrity_fsm_validation`**: Confirms that Free-Space Map page counts and free space entries match actual page states.

### 5. `test_storage_corruption` (`tests/test_storage_corruption.cpp`)
- **`test_detect_corrupted_page_checksum`**: Mutates bytes in page payloads to trigger CRC-32 checksum verification failures.
- **`test_detect_corrupted_slotted_page`**: Injects invalid free-space pointers exceeding `MAX_PAYLOAD_SIZE` (4072 bytes) into slotted page headers, verifying logical slotted-page bounds detection.
- **`test_detect_corrupted_wal`**: Truncates and damages WAL records and headers, asserting that WAL corruption is cleanly trapped without segfaults.

### 6. `test_wal_lifecycle` (`tests/test_wal_lifecycle.cpp`)
- **`test_full_lifecycle_checkpoint_truncate_recover`**: Complete lifecycle test executing high-volume inserts, taking a safe checkpoint, truncating the WAL, simulating a crash, and validating complete data integrity post-recovery.

---

## 3. Crash Simulator Verification (10 Points)

`CrashSimulator` testing validated deterministic crash resistance across 10 critical operational phases:

| Crash Point | Operational Step | Injected Failure | Recovery Behavior Verified |
|---|---|---|---|
| **Point 1** | WAL write during INSERT | Crash before WAL write | Page unchanged, transaction not logged, no corrupt state |
| **Point 2** | WAL write during INSERT | Crash after WAL, before disk flush | WAL recovers redo during crash recovery |
| **Point 3** | Buffer pool flush | Crash during dirty page write | WAL redo reconstructs missing page state |
| **Point 4** | Transaction COMMIT | Crash before COMMIT record flushed | Transaction treated as loser and cleanly undone |
| **Point 5** | Transaction COMMIT | Crash after COMMIT record flushed | Transaction treated as winner and cleanly redone |
| **Point 6** | Checkpoint Step 1 | Crash before WAL checkpoint record | Recovery falls back to prior checkpoint or log start |
| **Point 7** | Checkpoint Step 2 | Crash during dirty buffer pool flush | Pages partially flushed; subsequent recovery redos remaining |
| **Point 8** | Checkpoint Step 3 | Crash before Page 0 Checkpoint LSN update | WAL contains checkpoint; recovery uses WAL checkpoint boundary |
| **Point 9** | Checkpoint Step 4 | Crash during WAL temp file rewrite | Temp file discarded; original WAL remains valid |
| **Point 10** | Recovery Re-do Phase | Crash during recovery redo | Idempotent LSN checks prevent double application on restart |

---

## 4. Benchmark Performance Metrics

The long-running benchmark executable (`benchmark_wal_lifecycle.exe`) was executed against a relational database with multiple tables, foreign keys, and rollbacks:

### Empirical Results:
- **Workload:** 100 multi-operation transactions (86 committed, 14 aborted/rolled back).
- **Uncheckpointed WAL Size:** 3,808,560 bytes (~3.8 MB).
- **Post-Checkpoint WAL Size:** 92 bytes (down to 1 active checkpoint record).
- **WAL Storage Reduction:** **100.0% reduction** (~3.8 MB reclaimed).
- **Checkpoint Duration:** **54.16 ms** (including buffer pool flushes, fsync, and atomic truncation).
- **Startup Recovery Time:** **2.978 ms** (scanning 1 record, redone 0 pages, undone 0 transactions).
- **Storage Integrity Audit:**
  - Total Pages Checked: 13
  - Corrupted Pages: 0
  - Tables Checked: 2
  - Indexes Checked: 3
  - Records Audited: 83
  - FSM Pages: 1
  - Total Errors: 0
  - Total Warnings: 0 (Status: HEALTHY / CLEAN).
