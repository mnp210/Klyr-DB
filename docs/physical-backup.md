# KlyrDB Physical Snapshot Backup Engine

## 1. Architecture & Principles

The physical snapshot backup engine (`BackupManager`) captures consistent, point-in-time physical images of a live or offline KlyrDB database without requiring engine restarts or sacrificing ACID guarantees.

```
+-------------------------------------------------------------------+
|                        Source Database                            |
|  [PageFile (data.kdb)]  [WriteAheadLog (wal.log)]  [LockManager]  |
+-------------------------------------------------------------------+
                                  |
                                  | 1. Acquire Shared Lock (SWMR)
                                  | 2. Flush Dirty Pages & Sync Hardware
                                  | 3. Synchronize Checkpoint & Active LSN
                                  v
+-------------------------------------------------------------------+
|                        Staging Area                               |
|        <backup_dir>.tmp.<pid>.<timestamp>/                         |
|        - data.kdb (CRC32: 0x3F8A12B4)                             |
|        - wal.log  (CRC32: 0x89C1D0EE)                             |
|        - manifest.json                                            |
+-------------------------------------------------------------------+
                                  |
                                  | 4. Post-Backup Verification Check
                                  | 5. Atomic Publication (Rename)
                                  v
+-------------------------------------------------------------------+
|                    Published Backup Directory                     |
|                           <backup_dir>/                           |
+-------------------------------------------------------------------+
```

---

## 2. Execution Workflow

### Step 1: Concurrency Control & Snapshot Locks
The engine acquires:
1. **Cross-Process File Lock:** `ProcessFileLock` on `<db_path>.lock` in shared mode, ensuring external processes cannot truncate or mutate the database concurrently.
2. **In-Process Shared Lock:** `SharedLockGuard` via `LockManager`, blocking concurrent writers while allowing concurrent reader queries to continue unhindered.

### Step 2: Buffer Flushing & Boundary Synchronization
1. `BufferPoolManager::flush_all_pages()` flushes all in-memory modified frames to disk.
2. `PageFile::sync()` forces dirty OS filesystem cache pages onto persistent storage.
3. If WAL exists, `WriteAheadLog::flush()` ensures all committed log records are durable on disk.
4. The engine reads the WAL records to locate the latest `checkpoint_lsn` and the `min_required_lsn` of any active transactions.

### Step 3: Staged Component Duplication
Components are copied to a transient staging directory using canonical names:
- Database page file -> `data.kdb`
- WAL log file -> `wal.log`

### Step 4: IEEE 802.3 CRC-32 Checksumming
Each component is streamed in 64 KB chunks through the hardware-accelerated CRC-32 checksum routine to generate unambiguous bit-level integrity hashes.

### Step 5: Manifest Generation & Atomic Publication
The `BackupManifest` is serialized to `manifest.json`. Upon successful generation, the staging directory is atomically renamed to the target destination path.

---

## 3. CLI Usage

To generate a physical backup from the CLI:

```bash
# Standard backup
klyrdb backup /var/lib/klyrdb/sales.kdb /var/backups/sales_20261023

# Overwrite existing backup destination
klyrdb backup /var/lib/klyrdb/sales.kdb /var/backups/sales_20261023 --overwrite
```
