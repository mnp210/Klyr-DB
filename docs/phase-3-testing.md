# KlyrDB — Phase 3 Verification & Benchmark Report

## 1. Test Suite Summary

The KlyrDB test suite contains 9 test suites covering all architectural layers from raw physical page serialization to multi-level B+ tree indexing and Free-Space Map allocation.

All tests compile with `/W4 /permissive-` (MSVC) / `-Wall -Wextra -Wpedantic` and pass with 100% success.

| Test Executable | Target Layer | Test Scenarios | Status | Duration |
|-----------------|--------------|----------------|--------|----------|
| `test_page` | Physical Storage | Page header validation, CRC-32 integrity, payload limits | **PASSED** | 0.09s |
| `test_page_file` | Persistence Layer | File creation, append, offset writes, non-destructive create | **PASSED** | 0.10s |
| `test_persistence` | Recovery & Reopen | Database reopen, checksum validation, metadata persistence | **PASSED** | 0.14s |
| `test_buffer_pool` | Caching & Paging | Clock eviction, pin safety, dirty writebacks, victim selection | **PASSED** | 0.19s |
| `test_slotted_page` | Record Packing | Variable-length records, tombstoning, compaction, stability | **PASSED** | 0.09s |
| `test_record_manager`| Multi-Page Storage | Multi-page allocation, updates, deletions, typed tuples | **PASSED** | 0.24s |
| `test_bplus_tree` | Indexing Engine | Empty tree, duplicates, splits, merges, root collapse, iterators | **PASSED** | 0.29s |
| `test_fsm` | Space Management | FSM init, capacity search, chained pages, rebuild, reopen | **PASSED** | 0.11s |
| `test_index_integration` | End-to-End System | FSM + RecordManager + B+ Tree + Tiny buffer pool pressure | **PASSED** | 0.13s |

**Total Suite Execution Time:** ~2.07 seconds.

---

## 2. Detailed B+ Tree Test Matrix (`test_bplus_tree`)

1. **`EmptyTree`:**
   - Lookups on an empty tree return `StatusCode::KeyNotFound`.
   - Range scan returns an empty list without segfaulting.
   - `begin() == end()`.

2. **`SingleLeafOps`:**
   - 10 sequential insertions into a single leaf.
   - Point lookups verify exact `RecordId` mappings.
   - Range scans `[30, 70]` verify inclusive and exclusive boundaries.
   - Rejection of exact duplicate `(key, rid)` pairs with `StatusCode::DuplicateKey`.
   - In-leaf deletion and compaction.

3. **`DuplicateLogicalKeys`:**
   - Insertion of multiple distinct records sharing key `100`: `(100, [1:1])`, `(100, [1:3])`, `(100, [2:0])`.
   - `tree.find(100)` returns all 3 records sorted strictly by `(page_id, slot_id)`.
   - Deleting one duplicate preserves the remaining entries.

4. **`StringKeys`:**
   - Tests UTF-8 string keys ("alice", "bob", "charlie", "david", etc.).
   - Verifies lexicographical sorting, prefix lookup, and string range scans.

5. **`LeafSplittingAndLinks`:**
   - Configures `max_leaf_entries = 4`. Inserts 8 keys.
   - Forces leaf split and root promotion.
   - Verifies all keys found and sibling `next_page_id` / `prev_page_id` bi-directional links remain intact.

6. **`MultilevelTreeSplits`:**
   - Configures `max_leaf_entries = 4`, `max_internal_entries = 4`.
   - Inserts 100 randomly shuffled keys, forcing tree height to grow to 3+ levels.
   - Verifies point lookups for all 100 keys and verifies that an open range scan returns perfectly ordered keys 1..100.

7. **`DeletionsAndUnderflow`:**
   - Inserts 20 keys with `max_leaf_entries = 4`.
   - Deletes all odd keys, triggering leaf borrowing from siblings, leaf coalescing/merging, internal node merging, and root collapse.
   - Verifies that only even keys remain and range scans match expectations.

8. **`IteratorTraversal`:**
   - Traverses entire tree using `for (auto it = tree.begin(); it != tree.end(); ++it)`.
   - Tests `tree.lower_bound(key)`.

9. **`CatalogAndPersistence`:**
   - Registers index in `IndexCatalog`.
   - Flushes buffer pool, closes file, reopens from disk, reads catalog, and resumes B+ tree operations on restored root.

---

## 3. Empirical Benchmarks (`benchmark_index.exe`)

Benchmarking was executed on a database with **5,000 records** on standard disk storage with a 128-frame buffer pool.

### Benchmark Results Table

| Workload | Operations | Total Time | Throughput | Average Latency |
|----------|------------|------------|------------|-----------------|
| **B+ Tree Insertions (with Splits)** | 5,000 records | 363.93 ms | **13,736 ops/sec** | **72.80 $\mu$s** / insert |
| **Indexed Point Lookups (B+ Tree)** | 2,000 queries | 109.60 ms | **18,247 ops/sec** | **54.80 $\mu$s** / query |
| **Sequential Table Scan Baseline** | 50 queries | 252.73 ms | 197.84 ops/sec | 5,054.63 $\mu$s / query |
| **FSM Allocation Candidate Search** | 2,000 queries | 6.51 ms | **307,219 ops/sec**| **3.26 $\mu$s** / lookup |

### Key Findings
1. **Index Lookup Speedup:** The B+ Tree point lookup achieved **92.24x speedup** over sequential table scanning ($54.80\,\mu\text{s}$ vs $5,054.63\,\mu\text{s}$).
2. **Range Scan Scalability:**
   - 10 items: $59.87\,\mu\text{s}$ per scan.
   - 50 items: $141.33\,\mu\text{s}$ per scan.
   - 200 items: $408.34\,\mu\text{s}$ per scan.
   - 1,000 items: $2,018.04\,\mu\text{s}$ per scan ($2.01\,\mu\text{s}$ per record retrieved).
3. **FSM Efficiency:** Locating a candidate page with sufficient free space required only **$3.26\,\mu\text{s}$**, completely removing the disk I/O cost of searching for insertion pages.
