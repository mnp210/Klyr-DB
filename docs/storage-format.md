# KlyrDB Binary Storage Format Specification (Phase 3)

This specification defines the exact binary layout of KlyrDB database files on disk. Any conforming implementation can read or write compatible files.

## 1. Fundamentals

- **Page Size**: Exactly 4096 bytes (`0x1000`).
- **File Alignment**: All database files must have a size that is an exact multiple of 4096 bytes. Any file whose size is not a multiple of 4096 is considered truncated or corrupted and rejected.
- **Byte Order**: Big-Endian (Network Byte Order) for all multi-byte integer and floating-point fields.
- **Compiler Independence**: No C/C++ `struct` is directly dumped to disk. All fields are encoded and decoded using explicit bit-shift operations.

## 2. Page Binary Structure

Every 4096-byte page is divided into two distinct regions:
1. **Page Header**: The first 24 bytes (`0x00` - `0x17`).
2. **Payload Area**: The remaining 4072 bytes (`0x18` - `0xFFF`).

```text
+--------------------------------------------------------------------------+
|                            4096-Byte Page                                |
+-----------------------------------+--------------------------------------+
|     Page Header (24 bytes)        |       Payload Area (4072 bytes)      |
|  Magic | Ver | Type | ID | ...    |  Active payload [0..payload_size]    |
|  Offset 0x00 .. 0x17              |  Zero padding [payload_size..4072]   |
|                                   |  Offset 0x18 .. 0xFFF                |
+-----------------------------------+--------------------------------------+
```

### 2.1 Page Header Field Layout (24 bytes)

| Offset (Hex) | Offset (Dec) | Field Name | Data Type | Size (Bytes) | Description |
|---|---|---|---|---|---|
| `0x00` | 0 | `magic` | `uint32_t` | 4 | Magic signature: `0x4B4C5952` (ASCII "KLYR") |
| `0x04` | 4 | `version` | `uint16_t` | 2 | Page format version (currently `1`) |
| `0x06` | 6 | `page_type` | `uint16_t` | 2 | Page type code (see enum below) |
| `0x08` | 8 | `page_id` | `uint32_t` | 4 | Logical page ID (must match `file_offset / 4096`) |
| `0x0C` | 12 | `payload_size` | `uint16_t` | 2 | Number of valid payload bytes (`0 <= size <= 4072`) |
| `0x0E` | 14 | `flags` | `uint16_t` | 2 | Reserved page flags (set to `0x0000` in version 1) |
| `0x10` | 16 | `checksum` | `uint32_t` | 4 | CRC-32 checksum computed over the page |
| `0x14` | 20 | `page_lsn` | `uint32_t` | 4 | Log Sequence Number of last modifying transaction |

### 2.2 Page Types

| Code | Name | Description |
|---|---|---|
| `0x0001` | `Metadata` | Database header and system configuration (Page 0 only) |
| `0x0002` | `Data` | General raw data payload |
| `0x0003` | `Record` | Slotted / encoded typed records |
| `0x0004` | `Free` | Unallocated page available on the free list |
| `0x0005` | `BTreeLeaf` | B+ Tree leaf node storing sorted keys and RecordIds |
| `0x0006` | `BTreeInternal` | B+ Tree internal node storing separators and child pointers |
| `0x0007` | `FreeSpaceMap` | Free-Space Map page recording per-page available capacities |
| `0x0008` | `IndexCatalog` | Index metadata catalog tracking registered index roots |
| `0x0009` | `TableCatalog` | Persistent schema catalog tracking tables and columns |

### 2.3 Checksum Algorithm & Limitations

- **Algorithm**: Standard CRC-32 (IEEE 802.3 specification, reversed polynomial `0xEDB88320`).
- **Computation**: Computed over the entire 4096 bytes of the page, with the 4-byte `checksum` field (`0x10`..`0x13`) set to zero during calculation.
- **Accidental Corruption Detection**: CRC-32 detects all single-bit and double-bit errors, any odd number of errors, and burst corruptions up to 32 bits long. This protects against bit rot, filesystem tearing, and media read glitches.
- **Distinction from Cryptographic Verification**: CRC-32 is **NOT** a cryptographic hash (unlike SHA-256). It provides zero preimage or collision resistance against malicious tampering. Anyone can modify a page and craft a matching CRC-32. It is strictly an accidental corruption detection mechanism.

---

## 3. Database Metadata Page (Page 0)

Page 0 is always present in a valid KlyrDB file. Its Page Header contains `page_id = 0` and `page_type = 0x0001` (`Metadata`).
Its payload begins at offset `24` with the 48-byte Database Header:

| Offset (Hex) | Offset (Dec) | Field Name | Data Type | Size (Bytes) | Description |
|---|---|---|---|---|---|
| `0x00` | 0 | `db_magic` | `uint32_t` | 4 | Database signature: `0x4B444231` (ASCII "KDB1") |
| `0x04` | 4 | `db_version`| `uint16_t` | 2 | Database format version (`1`) |
| `0x06` | 6 | `page_size` | `uint16_t` | 2 | Database page size (`4096`) |
| `0x08` | 8 | `page_count`| `uint32_t` | 4 | Total allocated pages in file |
| `0x0C` | 12 | `free_list_head` | `uint32_t` | 4 | First free page ID (`0xFFFFFFFF` if none) |
| `0x10` | 16 | `user_version` | `uint32_t` | 4 | User-defined schema version number |
| `0x14` | 20 | `created_at` | `uint64_t` | 8 | Unix epoch timestamp of file creation (seconds) |
| `0x1C` | 28 | `fsm_page_id` | `uint32_t` | 4 | Logical page ID of Free-Space Map root (`0xFFFFFFFF` if none) |
| `0x20` | 32 | `catalog_page_id` | `uint32_t` | 4 | Logical page ID of Index Catalog (`0xFFFFFFFF` if none) |
| `0x24` | 36 | `table_catalog_page_id` | `uint32_t` | 4 | Logical page ID of Table Catalog (`0xFFFFFFFF` if none) |
| `0x28` | 40 | `reserved` | `uint8_t[8]`| 8 | Reserved for future expansion (zeroed) |

---

## 4. Slotted Page Binary Layout (`PageType::Record = 0x0003`)

Pages with `page_type = 0x0003` (`Record`) format their 4072-byte payload area into a slotted page:

```text
+-------------------------------------------------------------------------------+
|                       Page Payload Area (4072 bytes)                         |
+-------------------+--------------------+------------------+-------------------+
| Slotted Header    | Slot Directory     | Contiguous Free  | Record Payloads   |
| (8 bytes)         | (4 bytes per slot) | Space            | (grow downwards)  |
|                   | Slot 0, Slot 1...  |                  |                   |
| Offset 0 .. 7     | Offset 8 .. N      | Offset N .. FSP  | Offset FSP .. 4071|
+-------------------+--------------------+------------------+-------------------+
```

### 4.1 Slotted Page Header (8 bytes, offsets 0..7 of payload)

| Offset | Field Name | Type | Size | Description |
|---|---|---|---|---|
| `0x00` | `slot_count` | `uint16_t` | 2 bytes | Total slot directory entries |
| `0x02` | `free_space_pointer` | `uint16_t` | 2 bytes | Byte offset where record payloads start |
| `0x04` | `deleted_slot_count` | `uint16_t` | 2 bytes | Number of tombstoned slots in directory |
| `0x06` | `flags` | `uint16_t` | 2 bytes | Reserved flags (0x0000) |

### 4.2 Slot Directory Entry (4 bytes per slot)

Located at offset `8 + (slot_id * 4)` within the payload:

| Offset | Field Name | Type | Size | Description |
|---|---|---|---|---|
| `+0` | `record_offset` | `uint16_t` | 2 bytes | Offset in payload where record data begins |
| `+2` | `record_length` | `uint16_t` | 2 bytes | Length in bytes of the record |

### 4.3 Tombstone Representation
A deleted slot is represented by:
- `record_offset = 0xFFFF` (`SLOT_DELETED_OFFSET`)
- `record_length = 0x0000` (`SLOT_DELETED_LENGTH`)

---

## 5. B+ Tree Node Binary Layout (`0x0005` Leaf, `0x0006` Internal)

Both node types occupy standard 4096-byte pages.

### 5.1 Node Header (24 bytes at payload offset 0)

| Offset | Field Name | Type | Size | Description |
|---|---|---|---|---|
| `0x00` | `node_type` | `uint16_t` | 2 bytes | `0x0005` (Leaf) or `0x0006` (Internal) |
| `0x02` | `entry_count` | `uint16_t` | 2 bytes | Number of active entries in this node |
| `0x04` | `free_space_pointer` | `uint16_t` | 2 bytes | Downwards boundary offset of serialized data |
| `0x06` | `flags` | `uint16_t` | 2 bytes | Bit 0: Root flag (`0x0001`) |
| `0x08` | `parent_page_id` | `uint32_t` | 4 bytes | Parent logical page ID |
| `0x0C` | `next_or_p0` | `uint32_t` | 4 bytes | Leaf: `next_page_id` \| Internal: first child pointer $P_0$ |
| `0x10` | `prev_page_id` | `uint32_t` | 4 bytes | Leaf: `prev_page_id` \| Internal: reserved |
| `0x14` | `reserved` | `uint32_t` | 4 bytes | Alignment padding |

### 5.2 Offset Table & Entries
- **Offset Table:** Starts at offset 24, growing upwards. Each entry is a `uint16_t` offset pointing to a serialized entry.
- **Leaf Entry:** Key tag (`0x01` Int64, `0x02` String) + Key Payload + `RecordId` (4 bytes `page_id` + 2 bytes `slot_id`).
- **Internal Entry:** Key tag + Key Payload + Right Child `page_id_t` (4 bytes).

---

## 6. Free-Space Map Binary Layout (`0x0007`)

The Free-Space Map records per-page free-space summaries:

| Offset | Field Name | Type | Size | Description |
|---|---|---|---|---|
| `0x00` | `page_type` | `uint16_t` | 2 bytes | `0x0007` (`FreeSpaceMap`) |
| `0x02` | `next_fsm_page_id` | `uint32_t` | 4 bytes | Chained FSM page ID or `0xFFFFFFFF` |
| `0x06` | `base_page_id` | `uint32_t` | 4 bytes | Starting page ID mapped by this FSM page |
| `0x0A` | `page_count` | `uint16_t` | 2 bytes | Number of page summaries in this page |
| `0x0C` | `free_bytes[0..2029]` | `uint16_t[]` | 4060 bytes | Array of free space in bytes per page |

---

## 7. Table Catalog Binary Layout (`0x0009`)

The Table Catalog page stores table schema definitions and their attached index descriptors:

| Offset | Field Name | Type | Size | Description |
|---|---|---|---|---|
| `0x00` | `page_type` | `uint16_t` | 2 bytes | `0x0009` (`TableCatalog`) |
| `0x02` | `table_count` | `uint16_t` | 2 bytes | Total registered tables |
| `0x04` | `next_catalog_page_id` | `uint32_t` | 4 bytes | Next catalog page in chain or `0xFFFFFFFF` |
| `0x08` | `last_table_id` | `uint32_t` | 4 bytes | Monotonic counter for assigning unique table IDs |
| `0x0C` | `entries` | variable | N bytes | Packed sequence of variable-length table entries |

### 7.1 Table Entry Format
- `entry_len` (`uint16_t`): Total byte length of this table entry.
- `table_id` (`uint32_t`): Unique numeric table identifier.
- `name_len` (`uint16_t`) + `name_bytes` (canonical lowercase UTF-8).
- `column_count` (`uint16_t`).
- For each column:
  - `col_name_len` (`uint16_t`) + `col_name_bytes`.
  - `col_type` (`uint8_t`: `0x01` = `INTEGER`, `0x02` = `TEXT`).
- `index_count` (`uint16_t`).
- For each attached index:
  - `index_name_len` (`uint16_t`) + `index_name_bytes`.
  - `column_name_len` (`uint16_t`) + `column_name_bytes`.
  - `column_index` (`uint16_t`).
  - `root_page_id` (`uint32_t`).
  - `is_unique` (`uint8_t`: `0` = non-unique, `1` = unique).

---

## 8. Multi-Table Encoded Row Format

Rows inserted into slotted pages start with a 7-byte header followed by typed value payloads:

```text
+---------+--------------+--------------+-------------------------------------+
| Version | Table ID     | Column Count | Column Data Payloads...             |
| 1 byte  | 4 bytes (BE) | 2 bytes (BE) | tag + length + bytes                |
| [0]     | [1..4]       | [5..6]       | [7..N]                              |
+---------+--------------+--------------+-------------------------------------+
```

### Column Value Encodings
- **INTEGER (`0x01`)**: 1 byte tag (`0x01`) followed by 8 bytes signed `int64_t` in big-endian order (total 9 bytes).
- **TEXT (`0x02`)**: 1 byte tag (`0x02`) followed by 4 bytes `uint32_t` length prefix in big-endian order, followed by variable-length UTF-8 string bytes (total $5 + L$ bytes).

---

## 9. Write-Ahead Log (<dbname>.wal) Format

Companion WAL files begin with a 32-byte header followed by variable-length binary records:

```text
+---------------------------------------------------------------+
|  Magic "KWAL" (4B)  | Version (2B) | Flags (2B) | ChkptLSN(8B)|
+---------------------------------------------------------------+
|  Created Timestamp (8B)             | CRC-32 (4B)| Reserved(4B)|
+---------------------------------------------------------------+
```

Each log record contains:
- `lsn` (`uint64_t`, 8B)
- `prev_lsn` (`uint64_t`, 8B)
- `txn_id` (`uint64_t`, 8B)
- `type` (`uint8_t`, 1B)
- `page_id` (`uint32_t`, 4B)
- `before_len` (`uint16_t`, 2B)
- `after_len` (`uint16_t`, 2B)
- `before_data` (`before_len` bytes)
- `after_data` (`after_len` bytes)
- `checksum` (CRC-32, 4B)

