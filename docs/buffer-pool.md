# KlyrDB Buffer Pool Architecture (Phase 2)

## 1. Overview & Purpose

The **Buffer Pool** sits between the higher-level execution engine (Record Manager, Slotted Pages, and future B+ Tree) and the persistent disk storage layer (`PageFile`).

Without a buffer pool, every page read or record modification would incur immediate, expensive disk I/O. The `BufferPoolManager` introduces:
1. Fixed-capacity in-memory frame cache (default 64 frames, configurable down to minimal pools).
2. Page pinning/unpinning lifecycle ensuring active pages cannot be evicted while in use.
3. Clock replacement policy (second-chance algorithm) providing amortized $O(1)$ eviction.
4. RAII `PageGuard` abstraction preventing pin count leaks and dangling pointers.
5. Dirty-page tracking ensuring modified pages are safely flushed to disk before frame reuse or database shutdown.

---

## 2. Frame Architecture & Metadata

The buffer pool manages an array of `PageFrame` structures:

```text
+--------------------------------------------------------------------------+
|                               PageFrame                                  |
+---------------------+----------------------------------------------------+
| Field               | Description                                        |
+---------------------+----------------------------------------------------+
| page                | In-memory 4096-byte Page instance                  |
| page_id             | Logical database page ID (or INVALID_PAGE_ID)       |
| pin_count           | Number of active threads/guards holding this frame |
| is_dirty            | True if modified in memory and unwritten to disk   |
| is_valid            | True if currently hosting an active database page  |
| reference_bit       | Clock replacer usage flag for eviction selection   |
+---------------------+----------------------------------------------------+
```

### Free-Frame List & Page Table
- **Free-Frame List (`free_list_`)**: A vector of frame indices `[0 .. pool_size - 1]` representing empty, unmapped frames.
- **Page Table (`page_table_`)**: A hash map mapping logical database `page_id_t` to its current `frame_id_t`. Guarantees that a page is never loaded into multiple frames simultaneously.

---

## 3. Clock Replacement Algorithm

KlyrDB implements the **Clock (Second-Chance)** page replacement policy, the classic algorithm used in high-performance database engines like PostgreSQL and SQLite's pager.

```text
               [Frame 0] (pin=0, ref=0)  <-- Clock Hand
              /         \
    [Frame 3]             [Frame 1] (pin=1, ref=1)
              \         /
               [Frame 2] (pin=0, ref=1)
```

### Eviction Workflow:
1. When a new frame is needed and `free_list_` is empty, `find_victim_frame` starts sweeping from `clock_hand_`.
2. For each frame at `clock_hand_`:
   - If `!is_valid`: skip.
   - If `pin_count > 0`: skip (pinned pages can **never** be evicted).
   - If `pin_count == 0` and `reference_bit == true`:
     - Clear `reference_bit = false` (giving it a "second chance").
     - Advance `clock_hand_ = (clock_hand_ + 1) % pool_size`.
   - If `pin_count == 0` and `reference_bit == false`:
     - **Victim Found!** Evict this frame. Advance `clock_hand_`.
3. If all frames in the pool have active pins (`pin_count > 0`), the search terminates after at most two full rotations and returns `StatusCode::BufferPoolFull`.

### Frame Reuse & Dirty Flushing:
- If the victim frame has `is_dirty == true`, it is immediately written back to disk via `PageFile::write_page(frame.page)`.
- If disk I/O fails, the error is immediately propagated and the frame is **not** overwritten, preventing silent data loss.
- The old `page_id` is removed from `page_table_`.
- All frame metadata is reset before loading the new page.

---

## 4. Pin/Unpin Lifecycle & RAII `PageGuard`

Every call to `fetch_page(page_id)` or `new_page(out_page_id)` increments the frame's `pin_count` and sets `reference_bit = true`.

Manual pin tracking can lead to severe resource leaks (e.g., forgotten unpin calls causing permanent pool exhaustion). KlyrDB provides `PageGuard` to enforce exception-safe, scope-bound page lifetime management:

```cpp
{
    auto guard_res = bpm.fetch_page_guard(page_id);
    if (!guard_res.is_ok()) { /* handle error */ }
    
    auto& guard = guard_res.value();
    // Access page safely
    guard->mutable_payload()[0] = 0x42;
    guard.mark_dirty();
} // Scope exit: PageGuard destructor automatically calls bpm.unpin_page(page_id, is_dirty)!
```

### Safety Guarantees:
- **Move-only**: `PageGuard` cannot be copied, guaranteeing unique guard ownership.
- **Dangling pointer protection**: Moving or dropping a guard disarms it, resetting its internal pointer.
- **Refusal to evict**: As long as at least one `PageGuard` is live, `pin_count > 0`, making eviction impossible.

---

## 5. Operations Summary

| Method | Behavior | Error Conditions |
|---|---|---|
| `fetch_page(pid)` | Returns raw pointer to page; increments pin count | `InvalidPageId`, `BufferPoolFull`, `IoError`, `PageCorrupted` |
| `fetch_page_guard(pid)` | Returns RAII `PageGuard` wrapping page | Same as `fetch_page` |
| `new_page(out_pid)` | Allocates new page on disk and brings into pool pinned | `BufferPoolFull`, `IoError` |
| `new_page_guard(out_pid)`| Allocates new page and returns RAII `PageGuard` | Same as `new_page` |
| `unpin_page(pid, dirty)` | Decrements pin count; marks dirty if flag set | `InvalidPageId` (not in pool), `InvalidArgument` (pin count == 0) |
| `flush_page(pid)` | Writes page to disk if dirty; clears dirty bit | `IoError` |
| `flush_all_pages()` | Writes all dirty frames to disk and syncs file | `IoError` |
| `delete_page(pid)` | Removes unpinned page from pool and frees frame | `InvalidArgument` (if pinned) |
