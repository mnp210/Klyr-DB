# KlyrDB Reliability & Concurrency Stress Testing

## 1. Overview

KlyrDB Phase 11 introduces multi-threaded stress testing and simulated sudden-death crash injection (`tests/test_reliability_stress.cpp` and `tests/test_randomized_sql.cpp`) to verify engine stability, transactional consistency, and data integrity under sustained competing workloads.

---

## 2. Multi-Threaded Reader-Writer Concurrency Stress

### Design & Implementation
- **Worker Configuration**: 3 concurrent background reader threads querying records continuously, while a concurrent writer thread executes a succession of interleaved transactions.
- **Transaction Mix**: Writer executes 50 distinct transactions alternating between `COMMIT` and `ROLLBACK` of row updates.
- **Concurrency Guarantees**:
  - Buffer pool page frames are protected by shared/exclusive latches (`PageGuard`).
  - Catalog locks and table locks prevent data corruption during simultaneous read/write operations.
  - Transactions rolled back via `ROLLBACK` correctly restore slotted-page states without polluting subsequent operations.
- **Post-Stress Verification**:
  - After threads join, `IntegrityChecker::check_all()` runs full storage, checksum, slotted page, B+ tree index, and catalog audits to verify 100% database health (`is_healthy == true`, 0 errors).

---

## 3. Crash Recovery Stress Cycles

### Sudden-Death Simulation
To test engine recovery under catastrophic termination scenarios without relying on process crashes:
1. Initialize table and insert committed baseline data.
2. Begin a multi-statement transaction, inserting new rows and updating existing records.
3. Call `BufferPoolManager::drop_frames_without_flush()`. This immediately purges all dirty in-memory page frames from RAM without allowing them to write to disk.
4. Close file descriptors and simulate engine restart.
5. Invoke `RecoveryManager::recover()`:
   - Analysis Phase: Scan WAL from last checkpoint.
   - Redo Phase: Replay all committed transactions to ensure durability.
   - Undo Phase: Roll back uncommitted active transactions using before-images.
6. Verify database health using `IntegrityChecker`.
7. Repeat the entire cycle across 3 consecutive generational rounds.

---

## 4. Property-Based Randomized SQL Workload

Implemented in `tests/test_randomized_sql.cpp`:
- Generates 100 random multi-statement transaction sequences with arbitrary combinations of:
  - `INSERT` with varying column types.
  - `UPDATE` modifying subset of records.
  - `DELETE` removing targeted rows.
  - `BEGIN`, `COMMIT`, and `ROLLBACK`.
- Asserts that all relational constraints (primary key uniqueness, NOT NULL) are maintained at every step.
- Confirms that after rollback, table state matches an independent in-memory reference model.
