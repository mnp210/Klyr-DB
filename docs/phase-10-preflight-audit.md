# KlyrDB — Phase 10 Preflight Audit

**Date:** 2026-10-23  
**Status:** Audit Completed — Baseline Verified (35/35 Tests Passing)  
**Target Subsystems:** Physical Backup, Atomic Restore, Backup Verification, Logical SQL Export/Import, CSV Migration, Disaster Recovery

---

## 1. Baseline Build & Test Suite Verification

### 1.1 Toolchain & Environment
- **Compiler:** Microsoft Visual C++ Compiler (MSVC 19.51+), C++20 (`/permissive- /W4 /std:c++20`).
- **Build System:** CMake 3.20+, CTest runner.
- **Operating System:** Windows (Win32 APIs: `LockFileEx`, `FlushFileBuffers`, `MoveFileExW`).
- **Baseline Test Suite Execution:**
  - Total test suites: 35
  - Passed: 35 (100%)
  - Failed: 0
  - Total execution time: 11.52 sec

### 1.2 Baseline Test Inventory
1. `test_page`: Page serialization, endianness, header checks.
2. `test_page_file`: PageFile creation, expansion, I/O boundaries.
3. `test_persistence`: Multi-page persistence across process restarts.
4. `test_buffer_pool`: BufferPoolManager Clock replacement and pinning.
5. `test_slotted_page`: SlottedPage variable-length records, slot directory.
6. `test_record_manager`: RecordManager multi-page record CRUD and RecordIds.
7. `test_bplus_tree`: BPlusTree splits, merges, duplicates, range scans.
8. `test_fsm`: FreeSpaceMap allocation and tracking.
9. `test_index_integration`: Table, index, and FSM integration under pressure.
10. `test_sql_lexer`: SQL tokenization, identifiers, literals, comments.
11. `test_sql_parser`: Recursive-descent AST construction.
12. `test_table_catalog`: Persistent schema catalog serialization.
13. `test_sql_executor`: SQL execution and query results.
14. `test_wal`: Write-Ahead Log records, CRC-32 checksums, LSN sequencing.
15. `test_transaction`: Transaction lifecycle, before-images, rollbacks.
16. `test_recovery`: Analysis, Redo, and Undo passes.
17. `test_crash_recovery`: Crash simulator injection scenarios.
18. `test_constraints`: NOT NULL, PRIMARY KEY, UNIQUE, FOREIGN KEY enforcement.
19. `test_constraint_transactions`: Relational integrity under rollbacks.
20. `test_query_planner`: Cost-based access path generation.
21. `test_explain`: EXPLAIN QUERY PLAN format and validation.
22. `test_query_plan_execution`: Index seek, range scan, residual filter execution.
23. `test_query_optimization`: Unselective scan inversion and sorting elimination.
24. `test_database_locking`: In-process SWMR database lock coordinator.
25. `test_process_locking`: Cross-process OS file lock coordinator.
26. `test_buffer_pool_concurrency`: Concurrent multi-threaded page access.
27. `test_concurrent_reads`: Scalable multi-threaded read workloads.
28. `test_concurrent_writes`: Exclusive writer serialization and isolation.
29. `test_transaction_isolation`: Dirty-read avoidance and concurrency safety.
30. `test_checkpoint`: Safe checkpoint execution, dirty frame flushing, metadata updates.
31. `test_wal_retention`: Active transaction retention boundaries and safe truncation.
32. `test_checkpoint_recovery`: Accelerated startup recovery from checkpoint boundaries.
33. `test_integrity_check`: Multi-layer storage and constraint verification.
34. `test_storage_corruption`: Page CRC, slotted page, and WAL corruption detection.
35. `test_wal_lifecycle`: End-to-end multi-table WAL lifecycle and recovery.

---

## 2. In-Depth Subsystem Inspection

### 2.1 Storage Layer & Page Allocation
- **Page Size:** Fixed 4096 bytes (`PAGE_SIZE`).
- **Page 0 (Database Metadata):** Contains `magic` (`0x4B444231` - "KDB1"), `version` (1), `page_count`, `free_list_head`, `user_version`, `catalog_page_id`, `fsm_page_id`, `table_catalog_page_id`, and `checkpoint_lsn` at byte offset 40.
- **Physical vs Logical Pages:** The database file is an exact multiple of 4096 bytes. `PageFile::page_count()` reflects `metadata_.page_count`.
- **Page Checksums:** Every 4096-byte page has a CRC-32 checksum in its 24-byte header (`OFFSET_PAGE_CHECKSUM = 16`). Modifying any byte on disk without updating CRC-32 causes `Page::deserialize()` to return `Status::checksum_mismatch()`.

### 2.2 Buffer Pool & Dirty Page Flushing
- `BufferPoolManager` holds an in-memory frame cache.
- `flush_all_pages()` iterates through all frames and calls `page_file_.write_page(frame.page)` for any dirty frame.
- **Write-Ahead Rule Enforcement:** Before a dirty frame is flushed, `BufferPoolManager` enforces `wal_->flush_up_to(frame.page.page_lsn())`.
- Hardware synchronization is enforced via `PageFile::sync()`, which calls `FlushFileBuffers` on Windows.

### 2.3 WAL, Checkpoint Protocol & Truncation
- **WAL Structure:** Stored in `<database_path>.wal`. Begins with a 32-byte header containing `magic` (`0x4B57414C` - "KWAL"), version, flags, `checkpoint_lsn`, and CRC-32.
- **Checkpoint Record:** `LogRecord::make_checkpoint` stores `min_required_lsn`, active transaction count, and active transaction IDs.
- **Checkpoint Protocol (`CheckpointManager`):**
  1. Acquires `DatabaseLock` exclusively.
  2. Flushes WAL up to the current last LSN.
  3. Flushes all dirty buffer pool pages to disk.
  4. Calls `PageFile::sync()` for physical media durability.
  5. Appends the checkpoint record to the WAL and flushes.
  6. Updates `checkpoint_lsn` on the WAL header and in Page 0 metadata.
  7. Truncates WAL safely using atomic temp-file swap (`truncate_safe`).

### 2.4 Locking & Concurrency Model
- **Process-Level Locking (`ProcessFileLock`):** Managed via `<database_path>.lock`. On Windows, uses `LockFileEx` (`LOCKFILE_EXCLUSIVE_LOCK`) with overlap structures.
- **Database-Level Locking (`DatabaseLock`):** Provides Single-Writer / Multiple-Reader (SWMR) in-process thread synchronization:
  - `acquire_shared()` for readers.
  - `acquire_exclusive()` for writers and checkpoints.

### 2.5 Catalog, Schema & Constraints
- **Table Catalog (`TableCatalog`):** Stored on Page 1 (or designated catalog page). Tracks table IDs, table names, columns (`ColumnDef`: name and type), attached indexes (`TableIndexInfo`), and constraints (`TableConstraint`).
- **Supported Data Types:**
  - `INTEGER` (`int64_t`)
  - `TEXT` (`std::string`)
  - `NULL` (explicit `SqlType::Null` with `std::monostate`, distinct from `0` and `""`)
- **Supported Constraints:**
  - `NOT NULL`
  - `PRIMARY KEY` (single-column; automatically creates backing unique B+ tree index)
  - `UNIQUE` (single-column; automatically creates backing unique B+ tree index)
  - `FOREIGN KEY` (single-column; references parent table and column with RESTRICT/NO ACTION semantics)
- **Supported SQL Statements:**
  - `CREATE TABLE <name> (<col> <type> [constraints], ... [table_constraints]);`
  - `CREATE [UNIQUE] INDEX <idx_name> ON <tbl>(<col>);`
  - `DROP TABLE <name>;`
  - `DROP INDEX <idx_name>;`
  - `INSERT INTO <tbl> [(<cols>)] VALUES (<literals>);`
  - `SELECT <cols|*> FROM <tbl> [WHERE ...] [ORDER BY ...] [LIMIT ...];`
  - `UPDATE <tbl> SET <col> = <val> WHERE <predicates>;` (WHERE mandatory)
  - `DELETE FROM <tbl> WHERE <predicates>;` (WHERE mandatory)
  - `BEGIN`, `COMMIT`, `ROLLBACK`
  - `EXPLAIN [QUERY PLAN] <SELECT ...>;`

### 2.6 Read-Only Storage Integrity Checker (`IntegrityChecker`)
- Performs comprehensive non-destructive audits:
  - Page 0 metadata and physical page counts.
  - Page CRC-32 checksums across the entire database file.
  - Slotted page structure, free-space pointers, and directory bounds.
  - B+ tree ordering, child pointers, and sibling links.
  - Table catalog definitions and column data.
  - Schema constraints (`NOT NULL`, `PRIMARY KEY`, `UNIQUE`, `FOREIGN KEY` referential integrity).
  - Free-Space Map consistency.
  - WAL header and record integrity.

---

## 3. Snapshot Consistency & Safety Strategy

### 3.1 The "Fuzzy Snapshot" Problem
Simply copying `<dbname>` and `<dbname>.wal` with standard file-copy APIs while the database engine is active is unsafe:
1. **Torn Pages:** A 4096-byte page write from the buffer pool might occur simultaneously with an OS file copy, copying 2048 bytes of old data and 2048 bytes of new data.
2. **Buffer Inconsistency:** Committed transactions may have modified pages in the buffer pool that have not yet been written to the database file.
3. **WAL / DB Desynchronization:** If the database file is copied first, followed by the WAL file, the WAL may have advanced or truncated during the window, leaving an inconsistent state.

### 3.2 The KlyrDB Consistent Physical Snapshot Protocol
To guarantee an unambiguous, bit-for-bit consistent physical snapshot:
1. **Locking Barrier:**
   - `BackupManager` must acquire `DatabaseLock` exclusively (and `ProcessFileLock`) on the source database to pause all concurrent writers and ensure no transactions are actively modifying pages.
2. **Pre-Snapshot Checkpoint & Flush:**
   - Execute a safe checkpoint (`CheckpointManager::checkpoint(CheckpointMode::Safe)`).
   - This enforces WAL-before-data order: all dirty cached pages are flushed to the database file, followed by a hardware durability barrier (`PageFile::sync()`).
   - The checkpoint boundary is recorded in Page 0 and WAL header.
3. **Atomic Capture to Staging:**
   - Backups are written to a temporary staging directory (`<backup_path>.tmp`).
   - Both the database file (`<dbname>`) and WAL file (`<dbname>.wal`) are copied from the durable disk state into the staging directory.
4. **Component Checksums & Manifest:**
   - Calculate CRC-32 checksums of the staged database file and WAL file.
   - Record database metadata, page counts, page sizes, checkpoint LSN, timestamp, and component checksums into a versioned manifest (`backup_manifest.json`).
5. **Self-Verification Before Publication:**
   - Run `BackupVerifier` against the staged backup: verify manifest, check component sizes and CRC-32 checksums, validate Page 0 header, and run `IntegrityChecker`.
6. **Atomic Publication:**
   - Atomically rename the staging directory to the target `<backup_path>`.
   - If target exists, fail unless overwrite is explicitly requested.

---

## 4. Disaster Recovery & Safe Restore Strategy

### 4.1 Restore Workflow & Guarantees
1. **Pre-Validation:** Verify the backup artifact completely *before* touching the destination database. Reject corrupt, truncated, or incompatible backups immediately.
2. **Conflict & Lock Check:**
   - If the destination database file exists, verify whether `--force` / `overwrite = true` is set. If not, refuse to overwrite.
   - Verify that the destination path is not locked by another process (acquire test lock via `ProcessFileLock`).
3. **Staging Isolation:**
   - Restore all files into a temporary staging destination (e.g. `<dest_path>.restore.tmp`).
4. **Crash Recovery Execution:**
   - If the backup includes an active WAL file, initialize `RecoveryManager` on the staged database to run Analysis, Redo, and Undo passes, ensuring all committed transactions are reflected and any uncommitted crash residue is cleanly reverted.
5. **Post-Restore Integrity Audit:**
   - Run `IntegrityChecker` on the staged database to confirm 100% health (CRC-32 checksums, slotted pages, B+ trees, relational constraints).
6. **Publication:**
   - Use atomic replacement (`MoveFileExW` with `MOVEFILE_REPLACE_EXISTING` on Windows, or atomic directory rename) to replace destination.
   - If any step fails, remove staging files and leave the original destination completely untouched.

---

## 5. Logical SQL Export/Import Architecture

### 5.1 Export Format Specification
- Human-readable `.sql` script executed within a transaction (`BEGIN TRANSACTION; ... COMMIT;`).
- **Table DDL in Topological Dependency Order:**
  - Inspect foreign key constraints to build a directed acyclic graph (DAG) of table dependencies.
  - Emit parent tables before child tables so `CREATE TABLE` and `FOREIGN KEY` constraints succeed.
- **Data Insertion:**
  - Emit `INSERT INTO <tbl> (<cols>) VALUES (<values>);` statements.
  - Format values:
    - `NULL` -> `NULL`
    - `INTEGER` -> `-123`, `0`, `456`
    - `TEXT` -> `'escaped string'` (single quotes escaped as `''`, supporting Unicode and embedded newlines)
- **Secondary Indexes:**
  - Standalone secondary indexes (`CREATE INDEX ...`) emitted after all `INSERT` statements to maximize import throughput.

### 5.2 Import Workflow
- Incremental streaming parser processing statement-by-statement rather than loading huge SQL dumps into memory.
- Executes within an explicit transaction. If any statement or constraint fails, the transaction is rolled back, leaving the database clean.

---

## 6. CSV Export/Import Architecture

### 6.1 CSV Format & Ambiguity Resolution
- **Standard:** RFC 4180 compliant CSV.
- **Header:** Row 1 contains column names matching the table schema.
- **Quoting & Escaping:**
  - Fields containing commas, newlines, or quotes are enclosed in double quotes `"...""..."`.
- **The NULL vs Empty String Distinction:**
  - **Explicit NULL Policy:** Unquoted `\N` represents SQL `NULL` (following PostgreSQL/MySQL standard convention).
  - Quoted empty string `""` represents SQL empty TEXT `""`.
  - For `INTEGER` columns: unquoted empty or `\N` maps to `NULL` (or rejected if NOT NULL).
  - For `TEXT` columns: `\N` maps to `NULL`; `""` maps to empty string `""`.
- **Constraint Enforcement:** CSV import executes through `SqlExecutor` or shared constraint validator, strictly honoring `NOT NULL`, `PRIMARY KEY`, `UNIQUE`, and `FOREIGN KEY` rules.

---

## 7. Audit Conclusion & Next Steps

All architectural primitives (PageFile, DatabaseLock, ProcessFileLock, CheckpointManager, RecoveryManager, TableCatalog, IntegrityChecker, and SqlExecutor) provide a sound foundation for Phase 10. We will proceed to Stage 1 to implement the modular backup, restore, and migration subsystems.
