# KlyrDB — B+ Tree Indexing Engine

## 1. Overview & Architecture

KlyrDB implements a persistent, disk-backed, balanced **B+ Tree** indexing engine designed from first principles in C++20. All index nodes are backed by fixed-size 4096-byte pages managed through the KlyrDB buffer pool, preserving strict page-level CRC-32 checksum integrity and safe reference counting.

```text
                           +---------------------------+
                           |   Root (Internal Node)    |
                           |   [ P0 | Key1 | P1 | ... ]|
                           +-------------+-------------+
                                         |
                     +-------------------+-------------------+
                     |                                       |
          +----------v----------+                 +----------v----------+
          | Internal Node Level |                 | Internal Node Level |
          |   [ P0 | K | P1 ]   |                 |   [ P0 | K | P1 ]   |
          +----------+----------+                 +----------+----------+
                     |                                       |
       +-------------+-------------+           +-------------+-------------+
       |                           |           |                           |
+------v------+             +------v------+    +------v------+             +------v------+
|  Leaf Node  |<----------->|  Leaf Node  |<-->|  Leaf Node  |<----------->|  Leaf Node  |
| [K, RID]... |  prev/next  | [K, RID]... |    | [K, RID]... |  prev/next  | [K, RID]... |
+-------------+             +-------------+    +-------------+             +-------------+
```

### Key Capabilities
- **Strongly-typed keys:** Signed 64-bit integers (`int64_t`) and UTF-8 strings up to 128 bytes with strict total ordering.
- **Deterministic duplicate-key support:** Secondary indexes support multiple records with the same logical key by ordering entries as `(IndexKey, RecordId)`.
- **Dynamic node splits:** 50/50 balanced splits on capacity overflow with separator push-up to parent internal nodes.
- **Dynamic balancing and underflow merges:** Sibling redistribution and coalescing on deletion, with automatic root collapse when the root internal node becomes empty.
- **Fast point lookups:** $O(\log N)$ descent from root to leaf, combined with $O(\log K)$ binary search within each page's slotted offset table.
- **Sequential range scans:** Bi-directional sibling pointers (`next_page_id` / `prev_page_id`) enabling sequential leaf iteration without root re-traversals.

---

## 2. On-Disk Page Node Format

Both leaf and internal nodes reside within standard KlyrDB pages (`PageHeader` size: 24 bytes, usable payload: 4072 bytes). Node page types are `PageType::BTreeLeaf` (`0x0005`) and `PageType::BTreeInternal` (`0x0006`).

### 2.1 Node Header Layout (First 24 Bytes of Payload)

```text
Offset | Field Name       | Type     | Description
-------+------------------+----------+-------------------------------------------------------------
0      | node_type        | uint16_t | 0x0005 (BTreeLeaf) or 0x0006 (BTreeInternal)
2      | entry_count      | uint16_t | Number of entries stored in this node
4      | free_space_ptr   | uint16_t | Byte offset from payload start to start of downwards data
6      | flags            | uint16_t | Bit 0: BTREE_FLAG_ROOT (0x0001)
8      | parent_page_id   | uint32_t | Logical Page ID of parent internal node (or INVALID_PAGE_ID)
12     | next_or_p0       | uint32_t | Leaf: next_page_id | Internal: first child pointer (P0)
16     | prev_page_id     | uint32_t | Leaf: prev_page_id | Internal: reserved (0)
20     | reserved         | uint32_t | 4 bytes alignment padding (0)
```

### 2.2 Entry Layout & Slotted Storage

Similar to KlyrDB's slotted data pages, B+ tree nodes use a dual-direction layout:
1. **Offset Table (Grows Upwards):** Starting at payload offset 24, an array of 2-byte offsets (`uint16_t`) pointing to entries. Entries are maintained in sorted order within the offset table.
2. **Variable-Length Entries (Grows Downwards):** Data bytes are appended from the end of the payload (`4072`) towards the top.

#### Leaf Entry:
- `KeyType` (1 byte tag: `0x01` for Int64, `0x02` for String)
- Key data (8 bytes for Int64; 2 bytes length + UTF-8 payload for String)
- `RecordId`: `page_id` (4 bytes big-endian) + `slot_id` (2 bytes big-endian)

#### Internal Entry:
- `KeyType` (1 byte tag)
- Key data (8 bytes for Int64; 2 bytes length + UTF-8 payload for String)
- Right child `page_id_t` (4 bytes big-endian)

---

## 3. Strict Total Ordering & Duplicate Key Resolution

In database indexing, non-unique secondary indexes frequently store multiple entries for the same logical key (e.g., `department = "Engineering"`).

KlyrDB guarantees a strict total order by indexing the composite tuple `(IndexKey, RecordId)`. Because each `RecordId` is globally unique within a database file:
1. Two entries with identical `IndexKey` are ordered by `rid.page_id`.
2. Two entries with identical `IndexKey` and identical `rid.page_id` are ordered by `rid.slot_id`.
3. An exact duplicate `(key, rid)` pair is rejected with `Status::duplicate_key()`.

Binary search over the node offset table utilizes this strict order to locate the exact entry in $O(\log K)$ steps.

---

## 4. Node Splitting and Merging Algorithms

### 4.1 Leaf Split
1. When an insertion causes `entry_count >= max_leaf_entries` or exceeds available payload space, a new leaf page is allocated via `bpm.new_page_guard()`.
2. Sibling pointers are linked: `new_leaf.next = old_leaf.next`, `new_leaf.prev = old_leaf.id`, `old_leaf.next = new_leaf.id`.
3. Entries are partitioned at index `mid = total_entries / 2`.
4. Left leaf retains `[0 .. mid - 1]`; right leaf receives `[mid .. total_entries - 1]`.
5. The separator key pushed to the parent is `entries[mid].key`.
6. Leaf guards are unpinned before ascending, preventing pin accumulation under low buffer pool capacities.

### 4.2 Internal Split
1. When an internal node overflows, a new internal page is allocated.
2. The entry at `mid` is **pushed up** to the parent and omitted from both children.
3. The right internal node receives `first_child_id = entries[mid].child_id`.
4. Remaining entries `[mid + 1 .. total - 1]` are moved to the new internal node, and their parent pointers are updated.
5. If the root splits, a new internal root is allocated, increasing tree height by 1.

### 4.3 Deletions and Underflow
1. When an entry is deleted, `compact()` defragments the node payload.
2. If `entry_count < (max_entries / 2)` in a non-root node:
   - **Borrowing (Redistribution):** If an adjacent sibling has surplus entries, the boundary entry is borrowed and the parent's separator key is updated.
   - **Coalescing (Merging):** If siblings cannot lend, the node is merged into its sibling, the separator is removed from the parent, and the empty page is deallocated via `bpm.delete_page()`.
3. **Root Collapse:** When an internal root's entry count drops to 0, its sole child becomes the new root, decreasing tree height.

---

## 5. Sequential Range Scanning & Cursor

Range scans execute in two stages:
1. **Descent:** The tree is descended from root to the leaf containing `low_key` (or leftmost leaf if unbounded).
2. **Horizontal Scan:** The leaf is scanned starting at the lower bound index. When the leaf is exhausted, the scan seamlessly continues via `next_page_id` until `high_key` is exceeded or `INVALID_PAGE_ID` is encountered.

`BPlusTreeIterator` wraps this traversal in a standard C++ forward iterator supporting `operator*()`, `operator++()`, `operator==()`, and `lower_bound()`.
