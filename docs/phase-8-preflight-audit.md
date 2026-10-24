# KlyrDB Phase 8 — Preflight Concurrency & Locking Audit

**Date:** 2026-10-22  
**Baseline Test Status:** 23/23 CTest test suites passing (100% pass rate, 2.35s runtime).

---

## 1. Subsystem Thread-Safety Assessment

| Subsystem / Class | Current Thread-Safety | Concurrency Hazards & Data Races |
|---|---|---|
| **`PageFile`** | **Unsafe** (0 mutexes) | `std::fstream` file stream operations (`seekp`, `seekg`, `write`, `read`) are invoked concurrently. Seeking and writing from multiple threads corrupts file offsets and interleaves bytes. No OS-level file locking exists. |
| **`BufferPoolManager`** | **Unsafe** (0 synchronization primitives) | `page_table_` (`std::unordered_map`), `free_list_` (`std::vector`), and `frames_` are completely unguarded. Concurrent `fetch_page` or `unpin_page` produces data races on map buckets and `pin_count`. Concurrent evictions race on `clock_hand_`. |
| **`PageGuard` / `Page`** | **Unsafe** | RAII `PageGuard` decrements `pin_count` in destructor without atomic operations or locks. Multiple threads holding guards to the same page can read/write the underlying 4096-byte memory buffer without page latches. |
| **`WriteAheadLog`** | **Unsafe** | `last_lsn_` and `flushed_lsn_` are non-atomic integers. `append_record` writes to a shared `std::vector<uint8_t> write_buffer_` and `std::FILE*` without a mutex. Concurrent appends cause corrupted WAL records or duplicate LSNs. |
| **`TransactionManager`** | **Unsafe** | `active_txns_` map and `current_txn_` pointer are shared across threads without synchronization. Concurrent `begin()`, `commit()`, and `rollback()` calls will corrupt transaction state and before-image lists. |
| **`RecordManager` & `FreeSpaceMap`** | **Unsafe** | FSM page search and slotted page inserts assume exclusive access to page headers and slot directories. |
| **`BPlusTree` & Index Catalog** | **Unsafe** | B+ tree structural modifications (node splits, merges, root page updates) perform multi-page pointer updates without latches or locking. Traversal during a split can follow invalid sibling pointers. |
| **`TableCatalog`** | **Unsafe** | Catalog page lookups, table creation, and index additions modify persistent catalog pages without serialization. |
| **`SqlExecutor` & `QueryPlanner`** | **Unsafe** | Executors accessing the same database instance concurrently execute transactions, mutations, and table scans without synchronization. |

---

## 2. Cross-Process Database File Ownership

- **Current State:** `PageFile` opens files with `std::ios::in | std::ios::out | std::ios::binary`. On Windows, standard `fstream` sharing permits other processes to open the same file.
- **Risk:** Two separate OS processes can open the same `.kdb` database file. Both processes will maintain independent buffer pool caches, write conflicting pages, append out-of-order records, or simultaneously run crash recovery, leading to catastrophic disk-level database corruption.
- **Requirement:** OS-level file locking (via Windows `LockFileEx` / `UnlockFileEx` with cross-platform abstraction) must enforce mutual exclusion:
  - Exclusive lock on database file / companion lock file for writers / single-process ownership.
  - Rejection of conflicting process startups with `StatusCode::DatabaseBusy`.

---

## 3. Transaction Isolation & Data Visibility Hazards

1. **Dirty Reads:** In KlyrDB Phases 1–7, mutations immediately write changes into buffer pool page frames. If Transaction A begins and inserts/updates rows, those dirty pages reside in memory. A concurrent `SELECT` from Thread B reads those dirty frames directly. If Transaction A subsequently executes `ROLLBACK`, Thread B has read uncommitted data that never existed permanently.
2. **Non-Atomic Batch Visibility:** A multi-row `UPDATE` or `INSERT` modifies records sequentially. Readers running concurrently can observe a partial state where only 2 out of 5 rows have been updated.
3. **Index/Record Inconsistency:** During an `INSERT`, the record is written to a slotted page first, and then index entries are inserted into attached B+ trees. A concurrent reader could locate the slotted record via full scan before the index entry exists, or find an index key whose table record is not yet finalized.
4. **Resolution:** A strict **Single-Writer / Multiple-Reader (SWMR)** database-level lock model:
   - Writers acquire an **Exclusive Database Lock** (`DBLockMode::Exclusive`) across the entire transaction or mutation lifecycle.
   - Readers acquire a **Shared Database Lock** (`DBLockMode::Shared`).
   - While a writer holds the exclusive lock, all incoming readers block or wait with a bounded timeout (`StatusCode::LockTimeout`), preventing dirty reads, phantom data, and partial visibility.

---

## 4. Recovery & Shutdown Safety

- `RecoveryManager::recover()` rewrites database pages and resets WAL state. It must run strictly under an exclusive lock before any concurrent threads or processes are granted access.
- `PageFile::close()` must wait for all in-flight readers and writers to release pins and locks, preventing use-after-free or writing to closed streams.

---

## 5. Preflight Baseline Verification

All 23 existing unit and integration test suites pass with 100% success rate:
- Tests: 23 passed, 0 failed.
- Execution time: 2.35s.
- Baseline confirmed.
