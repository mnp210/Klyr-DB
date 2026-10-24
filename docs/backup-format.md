# KlyrDB Backup Format Specification

## 1. Overview

KlyrDB Phase 10 introduces a structured, self-contained physical backup format designed for high-integrity storage snapshots, standalone verification, and disaster recovery.

A physical backup directory consists of:
- `manifest.json`: Machine-readable metadata describing storage engine versions, physical geometry, LSN boundaries, and component checksums.
- `data.kdb`: Canonical byte-for-byte snapshot of the KlyrDB 4096-byte slotted-page database file.
- `wal.log` (optional): Canonical copy of the Write-Ahead Log corresponding to the point-in-time snapshot, if active at backup time.

---

## 2. Manifest Schema (`manifest.json`)

The manifest is encoded in standard JSON. Below is an annotated specification:

```json
{
  "backup_format_version": 1,
  "storage_format_version": 1,
  "klyrdb_version": "1.0.0",
  "created_at_utc": "2026-10-23T08:48:00Z",
  "source_db_name": "production.kdb",
  "page_size": 4096,
  "page_count": 158,
  "checkpoint_lsn": 4096,
  "min_required_lsn": 4096,
  "status": "Complete",
  "components": [
    {
      "name": "database",
      "filename": "data.kdb",
      "size_bytes": 647168,
      "crc32": "0x3F8A12B4"
    },
    {
      "name": "wal",
      "filename": "wal.log",
      "size_bytes": 70320384,
      "crc32": "0x89C1D0EE"
    }
  ]
}
```

### Field Descriptions

| Field | Type | Description |
|---|---|---|
| `backup_format_version` | integer | Schema version of the backup format. Current version: `1`. |
| `storage_format_version`| integer | Engine page file storage version. Current version: `1`. |
| `klyrdb_version` | string | Engine release tag / semver string (`1.0.0`). |
| `created_at_utc` | string | ISO 8601 UTC timestamp of backup completion. |
| `source_db_name` | string | Base filename of the original source database. |
| `page_size` | integer | Database page size in bytes (`4096`). |
| `page_count` | integer | Total physical pages in `data.kdb` (including page 0 metadata). |
| `checkpoint_lsn` | integer | Checkpoint LSN recorded on Page 0 and WAL header. |
| `min_required_lsn` | integer | Lowest active transaction LSN required for crash recovery. |
| `status` | string | Publication state: `"Incomplete"` during staging, `"Complete"` upon publication. |
| `components` | array | List of physical files constituting the backup. |

### Component Record Attributes

- `name`: Logical role of the file (`"database"`, `"wal"`, `"indexes"`).
- `filename`: Local filename relative to the backup directory (`"data.kdb"`, `"wal.log"`).
- `size_bytes`: Exact file size in bytes as reported by the OS filesystem.
- `crc32`: Hexadecimal representation (`0xXXXXXXXX`) of the IEEE 802.3 32-bit Cyclic Redundancy Check calculated over the entire file payload.

---

## 3. Atomic Publication Protocol

To prevent corrupted or partial backups from ever appearing valid:

1. **Staged Creation:** The backup is constructed inside a temporary directory named `<target_backup_dir>.tmp.<pid>.<timestamp>`.
2. **Component Copy & Hashing:** Database and WAL files are copied into the staging directory and streamed through the CRC-32 calculation engine.
3. **Draft Manifest:** Manifest is written with `"status": "Complete"`.
4. **Flush & Sync:** All files in staging are flushed to disk hardware (`FlushFileBuffers` / `fsync`).
5. **Atomic Directory Rename:** The staged directory is renamed to `<target_backup_dir>`. If the operation fails or is interrupted, the directory guard deletes the temporary files.
