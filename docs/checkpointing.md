# KlyrDB Checkpointing & WAL Lifecycle Management

## 1. Overview

In write-ahead logging (WAL) database architectures, uncheckpointed logs grow monotonically with every transaction commit, page update, and allocation. Without periodic checkpointing and log truncation:
1. Storage is wasted on historical page images that have already reached disk.
2. Startup recovery must scan and replay the entire history of the database from LSN 1, leading to intolerable restart times.
3. Uncoordinated truncation risks deleting uncommitted undo records or committed redo records, destroying ACID guarantees.

KlyrDB Phase 9 introduces a dedicated, verifiable Checkpoint Subsystem (`CheckpointManager`) and atomic safe log truncation (`WriteAheadLog::truncate_safe`) that together establish an explicit, durable recovery boundary.

---

## 2. The Checkpoint Protocol

Checkpointing in KlyrDB coordinates active transactions, the buffer pool, physical page persistence, and WAL durability:

```
+-----------------------------------------------------------------------------------------+
|                                    CHECKPOINT PROTOCOL                                  |
|                                                                                         |
| 1. Mutual Exclusion:                                                                    |
|    Acquire exclusive lock on DatabaseLock to ensure no concurrent writes or recoveries. |
|                                                                                         |
| 2. Active Transaction Analysis:                                                         |
|    - Collect all active transaction identifiers.                                        |
|    - Compute min_required_lsn = min(first_lsn of all active transactions).              |
|    - If no transactions are active: min_required_lsn = wal.last_lsn() + 1.              |
|                                                                                         |
| 3. WAL-Before-Data Enforcement:                                                         |
|    Ensure all logged modifications are flushed to disk up to max dirty page LSN.        |
|                                                                                         |
| 4. Flush and Sync Database Pages:                                                       |
|    - Flush all dirty cached pages from BufferPoolManager to disk.                       |
|    - Execute platform hardware sync (FlushFileBuffers / fsync) on the PageFile.        |
|                                                                                         |
| 5. Durable Checkpoint Log Record:                                                       |
|    - Append LogRecordType::Checkpoint with checkpoint_lsn, min_required_lsn, and        |
|      the active transaction list.                                                       |
|    - Flush and sync WAL.                                                                |
|                                                                                         |
| 6. Persist Checkpoint Boundary in Headers:                                              |
|    - Update WalFileHeader::checkpoint_lsn and durable sync.                             |
|    - Update Page 0 DatabaseMetadata::checkpoint_lsn and durable sync.                   |
|                                                                                         |
| 7. Atomic Safe WAL Truncation / Recycling:                                              |
|    - Discard records prior to min_required_lsn.                                         |
|    - Retain all records >= min_required_lsn needed by active transactions.              |
|    - Atomically replace WAL using atomic file rename (MoveFileExW / rename).            |
+-----------------------------------------------------------------------------------------+
```

---

## 3. Safe WAL Retention Rules

KlyrDB enforces strict invariants regarding which WAL records may be discarded:

| Transaction State | Active Records Needed? | Can Older Records Be Discarded? | Action Taken by Checkpoint |
|:---|:---|:---|:---|
| **No Active Transactions** | None | Yes (all records before checkpoint) | Truncate all records prior to checkpoint record; restart log at `checkpoint_lsn`. |
| **Active Uncommitted Transactions** | Yes (all records $\ge \text{min\_required\_lsn}$) | Yes (only records $< \text{min\_required\_lsn}$) | Retain active transaction records in new WAL file; discard older committed records. |
| **Aborted / Rolling Back Transactions** | Yes (undo images) | No | Preserved until rollback completes. |
| **Crash Mid-Checkpoint** | All | No | Temporary `.wal.tmp` is abandoned; original WAL remains 100% intact. |

---

## 4. Crash Consistency During Checkpoints

KlyrDB guarantees that a crash during any stage of checkpointing cannot lose acknowledged committed transactions or corrupt the recovery state:

1. **Crash Before Checkpoint**: Database restarts from previous checkpoint; all committed transactions in WAL are replayed.
2. **Crash During Dirty Page Flush**: WAL contains redo images for all committed transactions; recovery reapplies them idempotently.
3. **Crash After Pages Flushed Before Checkpoint Record Durable**: Pages on disk are up to date; WAL analysis re-verifies them; recovery is idempotent.
4. **Crash After Checkpoint Durable Before Truncation**: Recovery observes the valid checkpoint record and resumes from it.
5. **Crash During Truncation**: The temporary `.wal.tmp` was not yet atomically renamed over `.wal`. The complete original WAL is preserved.

---

## 5. CLI Usage

To trigger a manual checkpoint via the KlyrDB CLI:

```bash
klyrdb checkpoint <database_file.kdb>
```

Sample output:
```text
[+] Checkpoint completed successfully in 3.12 ms
    Checkpoint LSN:     148
    Min Required LSN:   149
    Active Txns:        0
    WAL Size Before:    482912 bytes
    WAL Size After:     32 bytes
```
