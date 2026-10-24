# KlyrDB — Write-Ahead Log (WAL) Specification

## 1. Overview and Objectives

The Write-Ahead Log (WAL) provides persistent, sequential logging of all transaction lifecycle events and database page modifications in KlyrDB. Its primary objectives are:

1. **Durability (D in ACID):** Guarantees that once a transaction commits, its modifications are permanently recorded in non-volatile storage and will survive crashes, power outages, and process terminations.
2. **Atomicity (A in ACID):** Enables uncommitted or aborted transactions to be completely undone via logged before-images, leaving no partial modifications in the database file.
3. **Write-Ahead Rule Enforcement:** Enforces the fundamental database constraint: **no dirty data page is ever written to the database file on disk until the log record describing the modification has been durably flushed to the log file**.
4. **Physiological / Page-Image Logging:** Logs full 4096-byte before- and after-images of modified pages alongside physical metadata, ensuring deterministic redo and undo passes that are resilient to slotted-page layout changes and B+ tree node splits.

---

## 2. WAL File Structure & Binary Encoding

The WAL is maintained in a companion file named `<database_path>.wal`. All integers are serialized using big-endian byte order (`encode_u16_be`, `encode_u32_be`, `encode_u64_be`) to maintain binary portability across host architectures.

### 2.1 File Header (32 bytes)

The WAL file begins with a 32-byte header initialized when the log file is created:

| Offset | Size (Bytes) | Field Name | Type | Description |
|---|---|---|---|---|
| `0` | `4` | `magic` | `uint32_t` | Magic identifier `0x4B57414C` (`KWAL`) |
| `4` | `2` | `version` | `uint16_t` | WAL format version (`1`) |
| `6` | `2` | `flags` | `uint16_t` | Format flags (reserved, defaults to `0`) |
| `8` | `8` | `checkpoint_lsn` | `uint64_t` | LSN of the most recent completed checkpoint |
| `16` | `8` | `created_timestamp` | `uint64_t` | Microseconds since Unix epoch when WAL was created |
| `24` | `4` | `checksum` | `uint32_t` | CRC-32 checksum of bytes `0..23` |
| `28` | `4` | `reserved` | `uint32_t` | Padding to maintain 32-byte alignment |

```text
+---------------------------------------------------------------+
|  Magic "KWAL" (4B)  | Version (2B) | Flags (2B) | ChkptLSN(8B)|
+---------------------------------------------------------------+
|  Created Timestamp (8B)             | CRC-32 (4B)| Reserved(4B)|
+---------------------------------------------------------------+
```

---

## 3. Log Record Architecture

Every WAL entry is serialized as a self-contained, variable-length binary record protected by a trailing CRC-32 checksum.

### 3.1 Record Types

| Value | Enumeration | Description |
|---|---|---|
| `0` | `None` | Invalid/Uninitialized record |
| `1` | `Begin` | Transaction start marker |
| `2` | `Commit` | Transaction commit marker (flushed durably) |
| `3` | `Abort` | Transaction rollback / abort marker |
| `4` | `PageUpdate` | Modification of a database page (contains before- & after-images) |
| `5` | `PageAlloc` | Allocation of a new page in the database file |
| `6` | `Checkpoint` | Checkpoint marker indicating dirty pages were flushed to disk |

### 3.2 Binary Record Layout

| Field | Size (Bytes) | Description |
|---|---|---|
| `lsn` | `8` | Log Sequence Number (monotonically increasing byte offset in log) |
| `prev_lsn` | `8` | Previous LSN written by this transaction (`0` for `Begin` or checkpoint) |
| `txn_id` | `8` | Unique 64-bit transaction identifier (`0` for checkpoints) |
| `type` | `1` | `LogRecordType` enum (1 byte) |
| `page_id` | `4` | Target page ID (`INVALID_PAGE_ID = 0xFFFFFFFF` for lifecycle records) |
| `before_len` | `2` | Byte length of before-image (`0` or `4096`) |
| `after_len` | `2` | Byte length of after-image (`0` or `4096`) |
| `before_data` | `before_len` | Full 4096-byte raw before-image of the page |
| `after_data` | `after_len` | Full 4096-byte raw after-image of the page |
| `checksum` | `4` | CRC-32 checksum computed over all preceding fields |

Total header overhead per record: **25 bytes** + payload + **4 bytes** checksum.

---

## 4. Log Sequence Numbers (LSN) & Write-Ahead Enforcement

### 4.1 LSN Generation
- Each log record is assigned an `lsn_t` (`uint64_t`) representing its exact starting byte offset in the WAL file.
- LSNs are strictly monotonically increasing.
- The `WriteAheadLog` tracks:
  - `last_lsn_`: The LSN of the most recently appended record.
  - `flushed_lsn_`: The highest LSN whose bytes have been durably written and flushed to physical storage.

### 4.2 Page LSN Integration
- Offset `20` of every 24-byte `PageHeader` contains a 4-byte `page_lsn` field (`uint32_t`).
- When a page is modified by an active transaction, its `page_lsn` is updated to `static_cast<uint32_t>(rec.lsn)`.
- The in-memory buffer pool frame also tracks the page's LSN.

### 4.3 The Write-Ahead Logging Rule
Before any dirty page $P$ is flushed to disk (whether during buffer pool clock eviction or explicit `flush_page(page_id)`):
```cpp
uint32_t p_lsn = frame.page.page_lsn();
if (wal_ != nullptr && p_lsn > wal_->flushed_lsn()) {
    Status s = wal_->flush_up_to(p_lsn);
    if (!s.is_ok()) return s;
}
page_file_.write_page(frame.page);
```
This guarantees that **the database file on disk can never contain modifications whose log records are not already durable in the WAL**.

---

## 5. Checkpointing & WAL Lifecycle Management (Phase 9)

In Phase 9, checkpointing establishes a durable, verifiable recovery boundary and coordinates safe WAL space reclamation:

### 5.1 Checkpoint Record Layout
The `Checkpoint` record (`LogRecordType::Checkpoint`) encodes:
1. `min_required_lsn` (8 bytes): The earliest LSN required for crash recovery (the lowest `first_lsn` among active transactions, or the checkpoint LSN if all transactions are committed).
2. `active_txns_count` (4 bytes): Number of active in-flight transactions.
3. List of `active_txn_id` (8 bytes each): Transaction IDs of transactions active at checkpoint time.

### 5.2 Safe Checkpoint Protocol (`CheckpointManager`)
1. **Acquire Subsystem Synchronization:** Prevents race conditions with concurrent transactions.
2. **Reconcile Active Transactions:** Reads active transaction states from `TransactionManager` and determines `min_required_lsn`.
3. **Flush Dirty Pages:** Flushes all dirty buffer pool frames to disk through `bpm_.flush_all_pages()`.
4. **Issue Hardware Sync:** Calls `PageFile::sync()` (`FlushFileBuffers` / `fsync`) to guarantee data page durability.
5. **Log Checkpoint Record:** Appends structured checkpoint record to WAL and flushes durably.
6. **Update Page 0 Metadata:** Persists `checkpoint_lsn` into Page 0 at byte offset 40 and flushes Page 0.
7. **Safe Truncation (`truncate_safe`):** Discards log records strictly prior to `min_required_lsn`. Truncation writes active records to `<dbname>.wal.tmp` and performs an atomic filesystem replace, eliminating torn truncation risks. All log space preceding the recovery boundary is reclaimed immediately.
