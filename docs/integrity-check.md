# KlyrDB Storage & Relational Integrity Verification

## 1. Overview

KlyrDB Phase 9 introduces a dedicated, read-only integrity verification subsystem (`IntegrityChecker`) and an interactive CLI command (`klyrdb integrity-check`).

The integrity checker performs exhaustive audits across all physical and logical database layers without modifying or attempting silent repairs. If corruption is found, detailed diagnostic information identifies the exact page ID, slot ID, table name, or index name affected.

---

## 2. Verification Coverage

The integrity checker inspects seven distinct layers of the KlyrDB engine:

```
+-----------------------------------------------------------------------------------------+
|                                    INTEGRITY SUITES                                     |
+-----------------------------------------------------------------------------------------+
| 1. Database Header (Page 0):                                                            |
|    - Magic signature verification (0x4B444231 = "KDB1")                                 |
|    - Database format version and 4096-byte page size validation                         |
|    - Physical page count vs metadata page count agreement                               |
+-----------------------------------------------------------------------------------------+
| 2. Page Bounds & Checksums (CRC-32):                                                    |
|    - Physical file length equals page_count * 4096 bytes                                |
|    - Validates stored CRC-32 against computed CRC-32 on every page                      |
|    - Validates page ID header equals physical file offset index                         |
|    - Validates PageType enum validity                                                   |
+-----------------------------------------------------------------------------------------+
| 3. Slotted-Page Record Boundaries:                                                      |
|    - Slot directory boundary checks: directory cannot overlap free space pointer        |
|    - Free space pointer within [SLOTTED_PAGE_HEADER_SIZE .. MAX_PAYLOAD_SIZE]           |
|    - Active record byte spans do not overlap each other or slot headers                 |
|    - Deleted slot count <= total slot count                                             |
+-----------------------------------------------------------------------------------------+
| 4. Catalogs & Schema Verification:                                                      |
|    - TableCatalog and IndexCatalog pages exist and deserialize cleanly                  |
|    - Table first_page_id pointers exist within physical file boundaries                 |
|    - Index root_page_id pointers exist within physical file boundaries                  |
+-----------------------------------------------------------------------------------------+
| 5. B+ Tree Invariants:                                                                  |
|    - Internal node keys strictly ascending: key[i] < key[i+1]                           |
|    - Leaf node keys non-decreasing: key[i] <= key[i+1]                                  |
|    - Leaf sibling link consistency: next_page_id pointer chains traverse cleanly        |
|    - Index record pointers (RecordId) point to valid table pages                        |
+-----------------------------------------------------------------------------------------+
| 6. Relational Constraints:                                                              |
|    - Primary Key Uniqueness: no duplicate primary key values within any table           |
|    - Unique Index Constraints: no duplicate entries in UNIQUE indexes                   |
|    - NOT NULL constraints: verify constrained columns contain no NULL values            |
|    - Foreign Key Integrity: child foreign keys reference existing parent primary keys    |
+-----------------------------------------------------------------------------------------+
| 7. WAL & Checkpoint Consistency:                                                        |
|    - WAL header magic ("WAL1"), version, and CRC-32 checksum                           |
|    - Monotonically increasing LSN sequencing across all valid log records               |
|    - Checkpoint LSN consistency between Page 0 and WAL header                           |
+-----------------------------------------------------------------------------------------+
```

---

## 3. Concurrency & Safety

1. **Read-Only**: Verification operations never alter in-memory or on-disk pages.
2. **Locking**: The integrity checker automatically acquires a `SharedLockGuard` (SWMR read lock) on the database. It will wait for active write transactions or checkpoints to complete, preventing races with in-flight mutations.
3. **Exit Codes**:
   - `0`: Database is completely healthy (clean).
   - `1`: Invocation / file I/O error (e.g. file not found).
   - `2`: Integrity violations or corruption detected.

---

## 4. CLI Usage & Examples

### Command Syntax:
```bash
klyrdb integrity-check <database_file.kdb>
```

### Healthy Output:
```text
======================================================================
                 KLYRDB STORAGE INTEGRITY REPORT                      
======================================================================
Status:                HEALTHY (CLEAN)
Total Pages Checked:   42
Corrupted Pages:       0
Tables Checked:        3
Indexes Checked:       2
Records Audited:       150
FSM Pages Audited:     1
WAL Checked:           VALID
Total Errors:          0
Total Warnings:        0
----------------------------------------------------------------------
All integrity checks passed with 0 errors and 0 warnings.
======================================================================
```

### Corrupted Output Example:
```text
======================================================================
                 KLYRDB STORAGE INTEGRITY REPORT                      
======================================================================
Status:                CORRUPTED / DEFECTS FOUND
Total Pages Checked:   42
Corrupted Pages:       1
Tables Checked:        3
Indexes Checked:       2
Records Audited:       150
FSM Pages Audited:     1
WAL Checked:           VALID
Total Errors:          2
Total Warnings:        0
----------------------------------------------------------------------
Issues Detected (2):
  [ 1] [ERROR] [Page] Page 12 : Page checksum mismatch (stored: 0x4B3A1C20, computed: 0x1A2B3C4D)
  [ 2] [ERROR] [Constraint] Page 18 ('employees') : Foreign key violation: value '999' does not exist in referenced table 'departments'
======================================================================
```
