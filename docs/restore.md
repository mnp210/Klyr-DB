# KlyrDB Atomic Physical Restore Engine

## 1. Architecture & Safe Restore Protocol

The `RestoreManager` restores a physical backup into an operational database while guaranteeing total safety:
1. **Pre-Restore Validation:** Standalone non-destructive verification ensures the source backup is uncorrupted before touching any destination files.
2. **Conflict Prevention:** Refuses to overwrite existing destinations unless `--force` is explicitly provided.
3. **Lock Enforcement:** Verifies that no other active processes or threads hold locks on the destination database.
4. **Staged Crash Recovery:** If the backup contains an active WAL, crash recovery (Analysis, Redo, Undo) runs inside an isolated staging directory.
5. **Post-Restore Integrity Validation:** Runs deep storage structure audits (Page 0, Slotted Pages, Catalogs, Indexes) on the recovered database before publication.
6. **Atomic File Swap:** Replaces destination files using atomic filesystem renames, moving any pre-existing database aside as `.old.<pid>` until the new database is safely seated.

```
+-------------------------------------------------------------------+
|                        Backup Artifact                            |
|                 /var/backups/sales_20261023/                      |
+-------------------------------------------------------------------+
                                  |
                                  | 1. BackupVerifier::verify()
                                  v
+-------------------------------------------------------------------+
|                 Destination Safety & Lock Check                   |
|                  Exclusive Lock on Target DB                      |
+-------------------------------------------------------------------+
                                  |
                                  | 2. Copy components to Staging
                                  v
+-------------------------------------------------------------------+
|                        Restore Staging Area                       |
|               <dest_dir>/<dest_name>.restore_tmp.<pid>/           |
|                                                                   |
|   3. Run Crash Recovery (Analysis -> Redo -> Undo)                |
|   4. Deep IntegrityChecker::check_all()                           |
+-------------------------------------------------------------------+
                                  |
                                  | 5. Atomic File Swap
                                  v
+-------------------------------------------------------------------+
|                     Operational Target Database                   |
|                      /var/lib/klyrdb/sales.kdb                    |
+-------------------------------------------------------------------+
```

---

## 2. Standalone Verification Engine (`BackupVerifier`)

The `BackupVerifier` audits backup directories without modifying any files or requiring the source database engine:

- **JSON Structure & Schema Check:** Validates format version compatibility (`backup_format_version == 1`).
- **Publication Check:** Confirms `"status": "Complete"`.
- **Component File Existence & Size:** Verifies that all registered files exist and match their recorded sizes.
- **Bit-Level CRC-32 Checksum Verification:** Recalculates CRC-32 hashes for all components and compares them bit-by-bit against the manifest.
- **Page 0 Metadata Geometry:** Verifies Page 0 magic (`0x4B4C5952`), page size (`4096`), and recorded page counts.
- **Catalog & Index Integrity:** Validates that catalog roots and secondary index trees are structural and traversable.

---

## 3. CLI Usage

### Standalone Backup Verification
```bash
klyrdb backup-verify /var/backups/sales_20261023
```

### Physical Database Restore
```bash
# Safe restore to new path
klyrdb restore /var/backups/sales_20261023 /var/lib/klyrdb/restored.kdb

# Force replace existing database
klyrdb restore /var/backups/sales_20261023 /var/lib/klyrdb/production.kdb --force
```
