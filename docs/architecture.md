# KlyrDB Architecture — Phase 11: Observability, Query Profiling & Reliability Engineering

## 1. Architectural Philosophy

KlyrDB is an educational relational database engine inspired by SQLite's layered systems architecture. SQLite's modular separation isolates the Virtual Database Engine (VDBE), B-Tree subsystem, Pager/Buffer Pool, and OS abstraction layer.

- **Phase 1** established fixed-size 4096-byte pages, CRC-32 integrity validation, and persistent file management.
- **Phase 2** introduced the in-memory buffer pool with a Clock eviction algorithm, slotted pages, and a record manager for variable-length rows.
- **Phase 3** implemented persistent, balanced **B+ Tree Indexing** (point lookups, duplicate keys, dynamic splits/merges, range iterators, index catalog) and a persistent **Free-Space Map (FSM)**.
- **Phase 4** introduced a functional SQL execution pipeline: **Table Catalog**, **Row Codec**, **Lexer**, **Recursive Descent Parser**, **Query Execution Engine**, and an interactive **SQL Shell**.
- **Phase 5** established ACID foundations with **Write-Ahead Logging (WAL)**, **Transaction Management** (`BEGIN`, `COMMIT`, `ROLLBACK`), write-ahead page flush enforcement, and a restartable **Crash Recovery Engine**.
- **Phase 6** implements **Schema Constraints & Relational Integrity**: `NOT NULL`, `PRIMARY KEY`, `UNIQUE`, `FOREIGN KEY` (with `RESTRICT`/`NO ACTION` referential integrity), DDL/index protection, typed `NULL` support, and multi-row atomicity.
- **Phase 7** introduces **Query Planning, Cost-Based Optimization & EXPLAIN**: dedicated `QueryPlanner` subsystem, physical access paths (`TableScan`, `IndexSeek`, `IndexRangeScan`), calibrated cost model, index-assisted `ORDER BY` elimination, early-exit `LIMIT` pushdown, and `EXPLAIN [QUERY PLAN]` inspection.
- **Phase 8** implements **Concurrency Control, Locking & Database Hardening**: process file locks, table/database shared-exclusive synchronization guards, and thread-safe buffer pool management.
- **Phase 9** implements **Checkpointing, WAL Lifecycle Management & Storage Reliability**: active transaction tracking, safe checkpoint boundaries, atomic WAL truncation, startup recovery optimization, and read-only multi-layer storage and relational integrity verification.
- **Phase 10** implements **Physical Backup, Atomic Restore, Logical Export/Import & Disaster Recovery**: self-contained physical snapshot backups with `manifest.json` and CRC-32 checksums, standalone `BackupVerifier`, transactional `RestoreManager` with staged crash recovery and atomic swap, DAG-ordered topological SQL dumps, and RFC 4180 CSV migration.
- **Phase 11** implements **Observability, Query Profiling, Fuzz Testing & Reliability Engineering**: thread-safe atomic runtime metrics, `DatabaseMetricsRegistry`, human-readable and JSON diagnostics, `EXPLAIN ANALYZE` with microsecond-resolution execution profiling, `TableStatisticsRegistry` for $O(1)$ fast cardinality estimation, property-based randomized transactions, SQL parser mutation fuzzing, and concurrent reader-writer stress testing.

```text
+-------------------------------------------------------------+
|                Interactive SQL Shell (REPL)                 |
|  - ASCII table formatter, dot commands (.help, .schema)     |
+-------------------------------------------------------------+
                               |
                               v
+-------------------------------------------------------------+
|                     SQL Parser & AST                        |
|  - SqlLexer (case-insensitive tokens, literals, comments)   |
|  - SqlParser (recursive descent, strict syntax validation)  |
|  - Constraints AST (NOT NULL, PRIMARY KEY, UNIQUE, FK)      |
|  - EXPLAIN [QUERY PLAN] statement AST                       |
+-------------------------------------------------------------+
                               |
                               v
+-------------------------------------------------------------+
|           Cost-Based Query Planner (Phase 7)                |
|  - QueryPlanner (Generates candidate plans & cost metrics)  |
|  - Physical Access Paths (TableScan, IndexSeek, RangeScan)  |
|  - Multi-predicate bounds consolidation & residual filters  |
|  - Order By satisfaction without sort & LIMIT pushdown      |
+-------------------------------------------------------------+
                               |
                               v
+-------------------------------------------------------------+
|               Query Execution & Constraint Engine           |
|  - SqlExecutor (Constraint pre-validation & plan execution) |
|  - Relational Integrity (Foreign Key RESTRICT / NO ACTION)  |
|  - Auto-backing unique B+ tree index synchronization        |
|  - TableCatalog (Persistent constraint metadata)            |
|  - SqlRow / SqlValue (NULL representation & row codec)      |
|  - Implicit & explicit transaction scoping                  |
+-------------------------------------------------------------+
                               |
                               v
+-------------------------------------------------------------+
|               Transaction & Recovery Subsystem              |
|  - TransactionManager (BEGIN, COMMIT, multi-page ROLLBACK)  |
|  - RecoveryManager (Analysis, Redo, Undo passes)            |
|  - CrashSimulator (9 deterministic injection points)        |
+-------------------------------------------------------------+
                               |
         +---------------------+---------------------+
         |                                           |
         v                                           v
+----------------------------------+  +-------------------------------+
|       Write-Ahead Log (WAL)      |  |        Record Manager         |
|  - Binary log records (LSNs)     |  |  - insert, get, update, delete|
|  - Full before/after page images |  |  - Persistent RecordId        |
|  - Checkpoints & durable sync    |  |  - BPlusTree Indexing         |
+----------------------------------+  +-------------------------------+
         |                                           |
         +---------------------+---------------------+
                               |
                               v
+---------------------------------------------+  +---------------------------+
|             Buffer Pool Manager             |  |      Free-Space Map       |
|  - In-memory PageFrame cache                |  |  - Per-page free space    |
|  - Clock replacement policy (Second-Chance) |  |  - Fast candidate search  |
|  - PageGuard RAII lifetime management       |  |  - Auto-chained FSM pages |
|  - Write-Ahead Rule flush enforcement       |  |  - Slotted rebuild        |
+---------------------------------------------+  +---------------------------+
          |                                       |
          v (in-memory bytes)                     v (disk I/O)
+---------------------------+       +-------------------------+
|        SlottedPage        |       |        PageFile         |
|  - Slotted Page Header    |       |  - Page 0 Metadata      |
|  - Stable Slot Directory  |       |  - 4096-byte page reads |
|  - In-place compaction    |       |  - 4096-byte writes     |
|  - Variable-length records|       |  - Appends & sync       |
+---------------------------+       +-------------------------+
                                                  |
                                                  v
                                    +-------------------------+
                                    |   OS Filesystem / Disk  |
                                    +-------------------------+
```

---

## 2. Component Breakdown

### 2.1 Common Layer (`klyrdb::common`)
- **`types.hpp`**: Fundamental primitives (`page_id_t`, `slot_id_t`, `lsn_t`, `PageType`, `INVALID_PAGE_ID`, `INVALID_SLOT_ID`). Includes page types: `Metadata = 0x0001`, `Data = 0x0002`, `Record = 0x0003`, `Free = 0x0004`, `BTreeLeaf = 0x0005`, `BTreeInternal = 0x0006`, `FreeSpaceMap = 0x0007`, `IndexCatalog = 0x0008`, `TableCatalog = 0x0009`.
- **`status.hpp` / `status.cpp`**: Rich, typed error-handling system via `StatusCode` and `Status` (including `TableNotFound`, `TableAlreadyExists`, `ColumnNotFound`, `DuplicateColumn`, `TypeMismatch`, `SyntaxError`, `LexerError`, `ConstraintViolation`, `UnsupportedSql`).
- **`result.hpp`**: `Result<T>` monadic value-or-error container based on `std::variant<T, Status>`.
- **`endian.hpp`**: Explicit big-endian serialization routines using bit manipulation.
- **`checksum.hpp` / `checksum.cpp`**: Table-driven CRC-32 (IEEE 802.3 standard) implementation.

### 2.2 Storage & Caching Layer (`klyrdb::storage`)
- **`page_constants.hpp`**: Storage constants (4096-byte page size, "KLYR" signature, 24-byte header size, byte offsets for Page 0 metadata including `fsm_page_id`, `catalog_page_id`, and `table_catalog_page_id`).
- **`page_header.hpp` / `page_header.cpp`**: 24-byte header encoding, decoding, and validation for all page types.
- **`page.hpp` / `page.cpp`**: Self-contained 4096-byte page container.
- **`metadata_page.hpp` / `metadata_page.cpp`**: Database header specification stored in Page 0.
- **`page_file.hpp` / `page_file.cpp`**: RAII-managed database file coordinator.
- **`buffer_pool_manager.hpp` / `buffer_pool_manager.cpp`**: In-memory frame manager with Clock replacement policy.
- **`page_guard.hpp`**: RAII wrapper for pinned pages.
- **`slotted_page.hpp` / `slotted_page.cpp`**: Variable-length record layout inside page payload.
- **`record_manager.hpp` / `record_manager.cpp`**: Multi-page record manager orchestrating record CRUD operations over the buffer pool and FreeSpaceMap.
- **`free_space_map.hpp` / `free_space_map.cpp`**: Persistent free space tracking.
- **`integrity_checker.hpp` / `integrity_checker.cpp`**: Multi-layer read-only integrity verification engine auditing Page 0, CRC-32 checksums, slotted-page headers, B+ tree invariants, table catalogs, foreign key constraints, FSM, and WAL.

### 2.3 Indexing Layer (`klyrdb::index`)
- **`bplus_tree.hpp` / `bplus_tree.cpp`**: Dynamic, balanced B+ Tree index with root promotion and sibling underflow coalescing.
- **`index_catalog.hpp` / `index_catalog.cpp`**: Persistent index descriptor registry.
- **`key.hpp`**: Strongly-typed `IndexKey` with strict total ordering.

### 2.4 SQL & Schema Layer (`klyrdb::catalog` & `klyrdb::sql`)
- **`table_catalog.hpp` / `table_catalog.cpp`**: Persistent schema registry stored on `PageType::TableCatalog` page tracking table names, column types, and attached index descriptors across reopens.
- **`value.hpp`**: `SqlValue` variant representing `INTEGER` (`int64_t`) and `TEXT` (`std::string`) with type-safe comparison operators and bidirectional conversion to `IndexKey`.
- **`row.hpp` / `row.cpp`**: Portable binary row codec prefixing each record with `table_id` to enable multiple relational tables to share the storage layer safely.
- **`token.hpp` / `lexer.hpp` / `lexer.cpp`**: Lexical analyzer supporting SQL keywords, case insensitivity, numbers, string escaping (`''`), and comments.
- **`ast.hpp` / `parser.hpp` / `parser.cpp`**: Recursive-descent parser producing strongly typed AST nodes with line/column diagnostic error reporting.
- **`executor.hpp` / `executor.cpp`**: Coordinates catalog lookups, B+ tree index acceleration, table scans, in-memory sorting, and storage writes with safety enforcement.
- **`sql_shell.hpp` / `sql_shell.cpp`**: Interactive CLI REPL with ASCII table rendering and SQLite-style dot-commands (`.help`, `.tables`, `.schema`, `.indexes`, `.quit`).

### 2.5 Transactions, WAL & Recovery Layer (`klyrdb::wal`, `klyrdb::txn`, `klyrdb::recovery`)
- **`log_record.hpp` / `log_record.cpp`**: Binary WAL record format (`Begin`, `Commit`, `Abort`, `PageUpdate`, `PageAlloc`, `Checkpoint`) with 25-byte header, full page before/after-images, and CRC-32 integrity validation.
- **`write_ahead_log.hpp` / `write_ahead_log.cpp`**: Companion `<dbname>.wal` log manager supporting durable append, `flush_up_to(lsn)`, and hardware sync (`FlushFileBuffers` / `fsync`).
- **`transaction.hpp` / `transaction_manager.hpp` / `transaction_manager.cpp`**: Manages transaction state (`Active`, `Committed`, `Aborted`), multi-page reverse rollback, and enforces write-ahead logging before page writes.
- **`recovery_manager.hpp` / `recovery_manager.cpp`**: Three-pass restartable crash recovery (Analysis, Redo, Undo) optimized to begin scanning at the active checkpoint boundary.
- **`checkpoint_manager.hpp` / `checkpoint_manager.cpp`**: Safe checkpointing protocol reconciling in-flight active transactions with dirty pages, establishing durable boundaries, updating Page 0 checkpoint LSN, and executing atomic WAL truncation.
- **`crash_simulator.hpp`**: Deterministic crash injection framework testing 10 critical failure scenarios across transaction and checkpoint lifecycles.

### 2.6 Backup, Restore & Migration Layer (`klyrdb::backup`)
- **`backup_manifest.hpp` / `backup_manifest.cpp`**: Versioned metadata container and JSON serializer capturing storage engine versions, physical geometry, LSN boundaries, component names, byte sizes, and CRC-32 checksums.
- **`backup_manager.hpp` / `backup_manager.cpp`**: Coordinates physical snapshot backups, acquiring shared read locks without blocking readers, flushing dirty pages, computing bit-level CRC-32 checksums, and atomically publishing backups.
- **`backup_verifier.hpp` / `backup_verifier.cpp`**: Standalone, non-destructive validation engine verifying manifest syntax, completion status, bit-level CRC-32 hashes, Page 0 geometry, and structural catalog/index integrity.
- **`restore_manager.hpp` / `restore_manager.cpp`**: Transactional disaster recovery engine enforcing pre-restore validation, exclusive locking, isolated staging crash recovery (Analysis -> Redo -> Undo), post-restore integrity auditing, and atomic file swaps.
- **`logical_exporter.hpp` / `logical_exporter.cpp`**: Analyzes foreign key relationships to construct dependency graphs, computes topological DAG table orders, formats typed SQL literals with distinct `NULL` keyword fidelity, and outputs portable SQL dumps.
- **`logical_importer.hpp` / `logical_importer.cpp`**: Streams SQL dump files through `StatementStreamReader`, executing DDL and transactional `INSERT` chunks into an isolated staging database with transactional rollback on error.
- **`csv_migrator.hpp` / `csv_migrator.cpp`**: RFC 4180 compliant CSV export and import engine supporting multiline embedded text, `\N` explicit NULL sentinels, and relational constraint validation.

### 2.7 Diagnostics & Observability Layer (`klyrdb::diagnostics`)
- **`metrics.hpp` / `metrics.cpp`**: Zero-overhead atomic runtime metrics accumulators (`DatabaseMetrics`) tracking Buffer Pool efficiency (hit ratio, evictions, allocation failures), Query execution (statements, latency, rows examined/returned, table scans vs index seeks/scans), Transactions (commits, rollbacks, lock waits/timeouts), WAL/Recovery (bytes written/flushed, checkpoints, recoveries, redo/undo pages), and Backup operations. Includes thread-safe `DatabaseMetricsRegistry`, immutable `DatabaseMetricsSnapshot`, and export to human-readable strings and RFC 8259 JSON.
- **`table_statistics.hpp` / `table_statistics.cpp`**: Distribution statistics collection engine analyzing row counts, page counts, null counts, distinct values, and min/max key bounds per column, with thread-safe caching in `TableStatisticsRegistry` for $O(1)$ fast query planner row estimation.

