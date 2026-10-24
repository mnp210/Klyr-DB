# KlyrDB Slotted Pages Architecture (Phase 2)

## 1. Overview & Purpose

Standard relational database rows have variable lengths (text columns, binary blobs, variable-length integers). To pack multiple variable-length records into a fixed 4096-byte page frame without internal fragmentation or pointer corruption, KlyrDB uses the **Slotted Page** architecture.

A slotted page divides its 4072-byte usable payload into three distinct zones:
1. **Header & Slot Directory**: Stored at the beginning of the payload, growing upwards.
2. **Record Payloads**: Stored at the end of the payload, growing downwards towards the slot directory.
3. **Contiguous Free Space**: The gap between the slot directory and the `free_space_pointer`.

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

---

## 2. Byte-Level Binary Layout

The slotted page structure resides entirely within the 4072-byte payload area (`[24 .. 4095]` of the full 4096-byte page frame).

### 2.1 Slotted Page Header (8 bytes)

| Offset | Field | Type | Size | Description |
|---|---|---|---|---|
| `0` | `slot_count` | `uint16_t` | 2 bytes | Total number of slots in directory (including deleted) |
| `2` | `free_space_pointer` | `uint16_t` | 2 bytes | Relative offset where active record data begins |
| `4` | `deleted_slot_count` | `uint16_t` | 2 bytes | Number of tombstoned (deleted) slots in directory |
| `6` | `flags` | `uint16_t` | 2 bytes | Reserved flags (set to `0x0000`) |

### 2.2 Slot Directory Entry (4 bytes per slot)

Located at offset `8 + (slot_id * 4)` within the payload:

| Relative Offset | Field | Type | Size | Description |
|---|---|---|---|---|
| `+0` | `record_offset` | `uint16_t` | 2 bytes | Byte offset in payload where record begins |
| `+2` | `record_length` | `uint16_t` | 2 bytes | Length in bytes of the record payload |

### 2.3 Tombstone Representation
A deleted slot is represented by the sentinel values:
- `record_offset = 0xFFFF` (`SLOT_DELETED_OFFSET`)
- `record_length = 0` (`SLOT_DELETED_LENGTH`)

---

## 3. Free Space & Capacity Calculations

- `SLOTTED_PAGE_HEADER_SIZE = 8`
- `SLOT_ENTRY_SIZE = 4`
- `MAX_PAYLOAD_SIZE = 4072`
- `MAX_SLOTTED_RECORD_SIZE = 4072 - 8 - 4 = 4060 bytes`

### Contiguous Free Space:
```text
directory_end = SLOTTED_PAGE_HEADER_SIZE + (slot_count * SLOT_ENTRY_SIZE)
contiguous_free_space = free_space_pointer - directory_end
```

### Total Free Space:
Total free space includes contiguous free space plus the dead space left by deleted records or shrinking updates:
```text
active_bytes = sum(slot.length for slot in active_slots)
total_free_space = MAX_PAYLOAD_SIZE - directory_end - active_bytes
```

---

## 4. Slot Reuse & Compaction Mechanics

### 4.1 Stable Slot Identifiers
A critical correctness requirement in relational storage engines is that external pointers (`RecordId = (page_id, slot_id)`) must remain **stable**.
When compaction runs:
- Active record bytes are defragmented and slid towards the bottom of the page (`4072` downwards).
- Each active slot's `record_offset` in the slot directory is updated to point to the new byte location.
- **The slot ID does not change!** Slot 0 remains slot 0, slot 2 remains slot 2. External references are never invalidated.

### 4.2 Deletion & Slot Recycling
When `delete_record(slot_id)` is invoked:
1. The slot is tombstoned (`offset = 0xFFFF, length = 0`).
2. `deleted_slot_count` is incremented.
3. The space previously occupied by the record remains dead space until compacted.
4. When a new record is inserted, KlyrDB scans for tombstoned slots and **reuses the deleted slot ID**, avoiding unnecessary expansion of the slot directory.

### 4.3 In-Place Compaction
When `total_free_space >= needed_space` but `contiguous_free_space < needed_space`, `compact()` is triggered:
1. A scratch buffer mirrors the page header.
2. Active records are copied consecutively starting at offset `4072` down to `new_fsp`.
3. Updated offsets are written to their original slot entries in the scratch directory.
4. Tombstoned slots are preserved in place with `offset = 0xFFFF`.
5. The compacted scratch buffer replaces the page payload.
