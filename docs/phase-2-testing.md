# KlyrDB Phase 2 — Testing & Verification Strategy

## 1. Overview & Scope

Phase 2 introduces three major new subsystems:
1. `BufferPoolManager` with Clock replacement policy and `PageGuard`.
2. `SlottedPage` with variable-length records, stable slot IDs, tombstones, and compaction.
3. `RecordManager` with multi-page record allocation, persistent `RecordId`s, and typed record integration.

The testing strategy preserves all Phase 1 regression suites while adding dedicated suites for every Phase 2 requirement.

---

## 2. Test Suite Matrix

### 2.1 Regression Suites (Phase 1)
- **`test_page` (10 tests)**: Big-endian integer bit-shifters, CRC-32 properties, page initialization, 4096-byte serialization/deserialization, boundary packing, magic/version/checksum rejection.
- **`test_page_file` (6 tests)**: Non-destructive creation, file size verification, sequential page append/read, truncation detection, in-place updates.
- **`test_persistence` (3 tests)**: Multi-page persistence roundtrips, on-disk single-bit corruption detection, versioned typed tuple persistence.

### 2.2 Buffer Pool Tests (`test_buffer_pool`, 8 tests)
- **`Fetch Page From Disk`**: Validates initial disk read into buffer pool frame and pin count increment.
- **`Fetch Cached Page`**: Asserts that subsequent fetches return the exact same in-memory frame without disk reads, with matching pin count increments.
- **`Dirty Page Flushing`**: Tests in-memory page mutation, dirty bit setting, and explicit page flushing to disk.
- **`Eviction Under Pressure`**: Configures a small 2-frame buffer pool with 3 pages. Exercises Clock replacement eviction and verifies clean page re-fetching.
- **`Refusal to Evict Pinned Pages`**: Pins all frames in a 2-frame pool; asserts that attempting to fetch another page fails with `StatusCode::BufferPoolFull`. Unpinning permits eviction.
- **`PageGuard RAII`**: Tests scope-bound lifetime, move semantics, automatic unpinning on destruction, and dirty flag propagation.
- **`Invalid Page ID`**: Validates out-of-range fetch rejection with `StatusCode::InvalidPageId`.
- **`Delete Page`**: Tests deleting unpinned pages and releasing frames back to the free list.

### 2.3 Slotted Page Tests (`test_slotted_page`, 10 tests)
- **`Empty Page Initialization`**: Asserts slotted page header invariants, 0 slots, initial free space pointer at 4072.
- **`Insert and Retrieve Multiple Records`**: Inserts 10 variable-length records and verifies byte-for-byte read fidelity.
- **`Empty Record`**: Validates 0-byte record insertion and retrieval.
- **`Maximum Size Record`**: Packs a 4060-byte record to the exact page limit and verifies retrieval.
- **`Page Capacity Boundary`**: Rejects records > 4060 bytes with `PayloadOverflow` and full pages with `NoFreeSpace`.
- **`Record Deletion Tombstone`**: Tests slot tombstoning and verifies `RecordNotFound` on deleted slot while adjacent slots remain intact.
- **`Slot Reuse After Deletion`**: Confirms that newly inserted records recycle existing deleted slot IDs.
- **`Record Update Shrink and Grow`**: Validates in-place shrinking updates and defragmented expanding updates.
- **`Compaction and Stable Slot IDs`**: Creates fragmentation by deleting interleaved slots, runs compaction, and confirms that active slot IDs and payloads remain completely stable.
- **`Invalid Slot ID`**: Asserts bounds checking for out-of-range slot indices.

### 2.4 Record Manager Tests (`test_record_manager`, 6 tests)
- **`Insert and Get Records`**: Tests inserting and reading multiple records on a single page.
- **`Records Distributed Across Pages`**: Inserts 10 x 1000-byte records, verifying multi-page allocation across disk pages.
- **`Update and Delete Record`**: Tests record modification and tombstoning through the record manager.
- **`Persistence After Close and Reopen`**: Inserts records across multiple pages, shuts down the buffer pool and file manager, reopens in a fresh session, and verifies persistence.
- **`Typed Record Integration`**: Inserts and retrieves structured `Record` tuples from Phase 1.
- **`Small Buffer Pool Thrashing`**: Sets pool size to 2 frames and thrashes across 20 large records on 5+ pages in random access order, confirming eviction resilience and zero data loss.

---

## 3. How to Run the Test Suite

```powershell
# Run all 6 test suites via CTest
ctest --test-dir build -C Debug --output-on-failure

# Or run individual test binaries
.\build\tests\Debug\test_page.exe
.\build\tests\Debug\test_page_file.exe
.\build\tests\Debug\test_persistence.exe
.\build\tests\Debug\test_buffer_pool.exe
.\build\tests\Debug\test_slotted_page.exe
.\build\tests\Debug\test_record_manager.exe
```
