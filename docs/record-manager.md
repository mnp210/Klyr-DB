# KlyrDB Record Manager Architecture (Phase 2)

## 1. Overview & Purpose

The `RecordManager` coordinates record-level storage operations across the database. It abstracts multiple slotted pages and the buffer pool into a unified record storage engine.

Key capabilities:
1. Translates `insert_record`, `get_record`, `update_record`, and `delete_record` requests into page-level operations.
2. Manages persistent record locators via `RecordId`.
3. Distributes records across existing pages and allocates new pages as capacity requires.
4. Seamlessly supports both opaque byte payloads (`std::span<const uint8_t>`) and structured typed `Record` tuples from Phase 1.

---

## 2. RecordId Semantics

A `RecordId` (RID) is a 6-byte compound identifier:
- `page_id_t page_id` (4 bytes, `uint32_t`): Logical disk page containing the record.
- `slot_id_t slot_id` (2 bytes, `uint16_t`): Slot index within the page's slot directory.

### Invariants & Guarantees:
- **Persistent across restarts**: `RecordId` directly indexes the physical slotted page layout on disk.
- **Stable across compactions**: Compacting a slotted page changes physical byte offsets on disk, but never alters slot directory indices. Thus, an acquired `RecordId` remains valid after defragmentation.
- **Tombstone Semantics**: When a record is deleted, accessing its `RecordId` returns `StatusCode::RecordNotFound`.
- **Slot Reuse Warning**: If a slot is tombstoned and a subsequent record insertion reuses that slot, a stale `RecordId` from prior to deletion will point to the new record. (In future phases with MVCC or transaction logs, epoch/generation counters can be added if generational ABA protection is required).

---

## 3. Allocation & Multi-Page Traversal

When inserting a new record:
1. `RecordManager` queries the total page count from `PageFile` via `BufferPoolManager`.
2. It scans existing record pages (`1 .. page_count - 1`), fetching each with a `PageGuard`.
3. If an existing slotted page reports `can_insert(record_size) == true`:
   - It inserts into that page.
   - Marks the page dirty.
   - Returns `RecordId{existing_page_id, slot_id}`.
4. If all existing pages are full (or no record pages exist yet):
   - Calls `bpm.new_page_guard(new_page_id)`.
   - Initializes the new page as a `SlottedPage` (`PageType::Record`).
   - Inserts the record into slot 0.
   - Marks dirty and returns `RecordId{new_page_id, 0}`.

---

## 4. Error Handling Matrix

| Scenario | Returned Status Code | Description |
|---|---|---|
| `RecordId` has `page_id == 0` | `InvalidPageId` | Page 0 is reserved for database header and system metadata |
| `RecordId` has `INVALID_PAGE_ID` | `InvalidArgument` | Uninitialized or malformed RecordId |
| Slot was previously deleted | `RecordNotFound` | The requested slot has been tombstoned |
| Slot index >= slot directory size | `InvalidSlotId` | Slot identifier out of bounds |
| Record size > 4060 bytes | `PayloadOverflow` | Record exceeds maximum capacity for a single page |
| Page lacks free space for growth | `NoFreeSpace` | Record expansion cannot fit on the current page |
| All buffer pool frames pinned | `BufferPoolFull` | Buffer pool cannot allocate or evict a frame |
