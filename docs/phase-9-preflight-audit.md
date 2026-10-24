# KlyrDB Phase 9: Preflight Audit — Checkpointing, WAL Lifecycle & Storage Reliability

## 1. Audit Scope & Objectives

Phase 9 addresses the long-term operational health and durability of KlyrDB:
1. Safe checkpointing protocol establishing a verifiable recovery boundary.
2. WAL retention and truncation without discarding records required by active transactions or dirty pages.
3. Startup recovery optimization starting from a validated checkpoint boundary.
4. Comprehensive read-only storage integrity verification (`klyrdb integrity-check`).
5. Deterministic crash recovery tests across 10 distinct failure points.
6. Empirical WAL growth and recovery performance measurements.

Baseline test pass rate: **29/29 tests passed (100%)** across Phases 1–8.

---

## 2. Component Audits

### 2.1 Write-Ahead Log (WAL) Format & Lifecycle
- **Header (`WalFileHeader`)**:
  - Magic: `0x57414C31` ("WAL1").
  - Version: `1`.
  - Fields: `page_size` (uint16_t), `checkpoint_lsn` (uint64_t), `last_lsn` (uint64_t), `checksum` (uint32_t CRC-32), reserved (4 bytes). Total size: 32 bytes.
- **Log Records (`LogRecord`)**:
  - Header: 48 bytes with magic `0x57414C52` ("WALR"), `type` (LogRecordType enum), `flags`, `lsn`, `prev_lsn`, `txn_id`, `page_id`, `before_image_len`, `after_image_len`, `checksum` (CRC-32).
  - Payloads: `before_image` (4096-byte full page image) and `after_image` (4096-byte full page image).
- **Append and Read Mechanics**:
  - `append_record`: Sequential append protected by `wal_mutex_`.
  - `flush`: Flushes file stream using `FlushFileBuffers` on Windows and `fsync` on POSIX.
  - `read_all_records`: Scans from byte 32 (after header) to EOF. Tolerates trailing incomplete records from ungraceful crash, but stops on corrupted mid-log records.

### 2.2 Existing Checkpoint Limitations & Safety Hazards
The Phase 5 baseline checkpoint implementation in `TransactionManager::checkpoint()` exhibits several critical safety hazards:
1. **Unsafe Blind Truncation**:
   - `TransactionManager::checkpoint()` calls `bpm_.flush_all_pages()`, appends a `Checkpoint` log record, flushes the WAL, and then calls `wal_.truncate(chk_lsn)`.
   - `wal_.truncate()` closes and reopens the WAL file with `"w+b"`, wiping all previous records.
   - **Hazard A (Active Transactions)**: If an uncommitted transaction $T_1$ modified pages in the buffer pool, `flush_all_pages()` flushes $T_1$'s uncommitted pages to disk! When the WAL is truncated, $T_1$'s undo images in the WAL are deleted. A subsequent crash leaves uncommitted data permanently on disk with zero ability to roll back.
   - **Hazard B (Non-Atomic Truncation)**: Recreating the file via `"w+b"` leaves a temporal vulnerability window where the WAL is 0 bytes. A crash at that instant loses the log entirely.
2. **Missing Checkpoint Metadata on Page 0**:
   - Page 0 `DatabaseMetadata` stores table and catalog roots, but did not store `checkpoint_lsn`. The 8 reserved bytes in `DatabaseMetadata` (`OFFSET_DB_RESERVED = 40`) can and should be used to record the durable `checkpoint_lsn`.
3. **No Active Transaction Tracking in Checkpoints**:
   - Checkpoint log records only stored `checkpoint_lsn` without an active transaction table or dirty page list, preventing checkpointing while long-running transactions are active.

### 2.3 Recovery Manager Audit
- Current `RecoveryManager::recover()` executes three passes:
  1. **Analysis Pass**: Scans every log record in the WAL from start to end, collecting commit/abort sets and finding `last_checkpoint_lsn`.
  2. **Redo Pass**: Scans forward, skipping records where `rec.lsn < last_checkpoint_lsn`. Redoes all page modifications for committed transactions where `guard->page_lsn() < rec.lsn`.
  3. **Undo Pass**: Scans backward, undoing uncommitted transactions where `guard->page_lsn() >= rec.lsn`.
  4. **Flush & Sync**: Flushes all pages to disk and appends a checkpoint record.
- **Optimization Opportunity**: If the WAL header and Page 0 contain a verified checkpoint boundary with no active transactions, the Analysis Pass does not need to read from LSN 1. It can resume directly from the checkpoint boundary, dramatically accelerating startup recovery on large logs.

### 2.4 Storage Integrity Verification Audit
- KlyrDB currently lacks a unified integrity-check tool to audit:
  - Database header and Page 0 invariants.
  - Page checksums (CRC-32) across all allocated database pages.
  - Slotted page slot offsets, lengths, overlaps, and free space consistency.
  - B+ tree key ordering, child bounds, and leaf sibling pointer chains.
  - Table Catalog and Index Catalog referential integrity.
  - Foreign key constraints between parent and child tables.
  - Free-Space Map (FSM) accuracy.
  - WAL header, record chaining, and checkpoint consistency.
- A read-only `IntegrityChecker` subsystem and CLI command (`klyrdb integrity-check <db_path>`) must be implemented with detailed reporting and zero side-effects.

---

## 3. Phase 9 Architecture & Safety Invariants

```
+---------------------------------------------------------------------------------+
|                               CHECKPOINT SUBSYSTEM                              |
|                                                                                 |
| 1. Acquire Database Exclusive Lock (DatabaseLock::acquire_exclusive)            |
| 2. Compute Safe Recovery Boundary:                                              |
|    - If active transactions exist: min_required_lsn = min(active_txns.first_lsn)|
|    - If no active transactions: min_required_lsn = wal.last_lsn() + 1           |
| 3. WAL-Before-Data Enforcement: flush WAL up to max dirty page LSN              |
| 4. Flush all dirty pages to database file & hardware sync PageFile              |
| 5. Append Checkpoint record with active txn table & min_required_lsn            |
| 6. Update WalFileHeader & Page 0 DatabaseMetadata with durable checkpoint LSN   |
| 7. Atomic Safe WAL Truncation / Recycling:                                      |
|    - Retain all log records >= min_required_lsn via atomic temp-file replace    |
+---------------------------------------------------------------------------------+
```

### Safety Invariants:
1. **WAL-Before-Data**: No database page is written to disk until all log records up to that page's `page_lsn` are durably flushed.
2. **No Data Loss on Truncation**: No WAL record with $\text{LSN} \ge \text{min\_required\_lsn}$ is ever deleted during checkpointing.
3. **Atomic File Replacement**: File truncation uses atomic rename (`MoveFileExW` / POSIX `rename`) so a crash never leaves a 0-byte or corrupted WAL.
4. **Read-Only Integrity Checking**: Verification never alters database pages or bypasses reader locks.
