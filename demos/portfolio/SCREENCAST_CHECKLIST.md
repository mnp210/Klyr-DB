# 60–120 Second Screen-Recording Checklist

A structured presenter guide for recording a concise, high-impact technical portfolio walkthrough of KlyrDB v0.12.0.

---

## Pre-Recording Setup (1–2 minutes before recording)

- [ ] **Terminal Setup:**
  - Font: Cascadia Code, JetBrains Mono, or Consolas (16–18pt).
  - Terminal size: $1920 \times 1080$ (16:9), clean dark background.
  - Path: `D:\KlyrDB`
- [ ] **Executable Verification:**
  - Run `.\build_audit_release\Release\klyrdb.exe version` once to verify path and dependencies.
- [ ] **Recording Tool:**
  - OBS Studio / Loom configured to record full terminal window at 1080p60.

---

## Timed Cue Sheet (90-Second Walkthrough)

| Timestamp | Screen Action | Spoken Commentary |
|:---:|---|---|
| **0:00 – 0:15** | Run `klyrdb.exe version` in terminal | *"KlyrDB is an embedded relational database engine implemented from scratch in standard ISO C++20. It features 4096-byte slotted-page storage, B+ tree indexing, and an ARIES-style crash recovery subsystem."* |
| **0:15 – 0:35** | Run `./demos/portfolio/run_demo.ps1` (Steps 1–3 appear) | *"Here we run our automated portfolio runner. In Step 1 and 2, it initializes a clean database file, creates a table with primary key and NOT NULL constraints, and seeds initial records into binary slotted pages."* |
| **0:35 – 0:55** | Highlight Step 4 (`EXPLAIN ANALYZE` output) | *"In Step 4, we create a secondary B+ tree index on `price`. When we run `EXPLAIN ANALYZE`, KlyrDB's cost-based query planner selects an Index Range Scan access path, reporting microsecond planning and execution telemetry."* |
| **0:55 – 1:15** | Highlight Step 5 (Rollback vs Commit) | *"Step 5 demonstrates transaction rollback atomicity. We begin a transaction, insert uncommitted record 999, and trigger a ROLLBACK. Notice the immediate re-query returns 0 rows. A subsequent valid insert commits cleanly."* |
| **1:15 – 1:35** | Highlight Steps 6 & 7 (Persistence & Integrity Check) | *"In Step 6, an independent CLI process spawns, reopening the database file to confirm all committed data persisted. In Step 7, KlyrDB's multi-layer integrity checker validates page CRC-32 checksums and slotted-page headers, confirming zero errors."* |
| **1:35 – 1:50** | Show `demos/portfolio/demo_sdk.cpp` snippet in editor | *"Beyond the CLI, KlyrDB provides a clean public C++20 SDK with RAII `TransactionGuard` handles and CMake package export support. The code is tested across 51 automated CTest test suites."* |

---

## What This Demo Proves vs Does Not Prove

To maintain engineering credibility and integrity during technical interviews:

### What the Demo Proves:
1. **Real Functionality:** Real execution of slotted-page storage, B+ tree indexing, query execution, cost-based planning, and transaction logging.
2. **Software Craftsmanship:** Complete, reproducible automation with structured error handling, zero external dependencies, and exit-code validation.
3. **Observability:** Built-in physical plan inspection (`EXPLAIN`), runtime profiling (`EXPLAIN ANALYZE`), and offline integrity checking (`integrity-check`).

### What the Demo Does Not Prove:
1. **Crash Recovery Correctness:** Demonstrating in-memory `ROLLBACK` on an uncommitted transaction does **not** prove power-loss crash recovery or ARIES undo/redo correctness (those are validated separately in `test_crash_recovery` with 10 deterministic crash-injection scenarios).
2. **Concurrent Write Scalability:** Demonstrating sequential DML does **not** imply concurrent multi-writer throughput; KlyrDB enforces single-writer / multi-reader (SWMR) concurrency at the database level.
3. **Arbitrary Complex SQL:** Demonstrating single-table queries with filtering and ordering does **not** imply support for `JOIN`, subqueries, or aggregate expressions.
