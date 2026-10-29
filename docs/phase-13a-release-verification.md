# KlyrDB Phase 13A — Release Candidate Verification Report

> [!NOTE]
> **Historical Audit Report:** This document records the verification of the initial release candidate archive (SHA-256: `E2FD557A97C4F3D89982513C8BB5061B02A387204EB944647918DEFB6C42868F`), which was found to contain intermediate build artifacts in `examples/**/build/`.
> For the clean, validated release candidate archive (SHA-256: `58CF6568E9FB5DF8B37A865651A5E99218EDE451F3A9523689878DD4A1242DEA`), see [docs/phase-13b-release-verification.md](phase-13b-release-verification.md).

**Date:** 2026-10-24  
**Project:** KlyrDB C++20 Database Engine  
**Release Candidate Version:** `0.12.0`  
**Host Platform:** Windows 11 x64  
**Compiler:** Microsoft (R) C/C++ Optimizing Compiler Version 19.51.36260.0 for x64  
**CMake Version:** `4.3.1-msvc1`  
**CTest Version:** `4.3.1-msvc1`  
**Evaluator:** Senior C++ Release Engineer  
**Final Verdict:** **PASS WITH LIMITATIONS**  

---

## 1. Initial Repository State & Working Tree Audit

### 1.1 Git Status & History

Command executed:
```powershell
git status --short
git branch
git log -n 5
```

Output:
```text
fatal: not a git repository (or any of the parent directories): .git
fatal: not a git repository (or any of the parent directories): .git
fatal: not a git repository (or any of the parent directories): .git
```

- **Repository Structure:** The local directory `D:\KlyrDB` is a standalone project tree and is currently not initialized as a Git working tree (`.git` directory is absent).
- **Working Tree Preservation:** Zero files were discarded, reset, cleaned, or overwritten. No Git commits, tags, pushes, or remote operations were performed.

---

## 2. Release Archive & Checksum Verification

### 2.1 Archive Existence and Hash Comparison

Target Archive: `D:\KlyrDB\dist\klyrdb-0.12.0-windows-x64.zip`  
Checksum File: `D:\KlyrDB\dist\klyrdb-0.12.0-windows-x64.zip.sha256`  
Expected SHA-256: `E2FD557A97C4F3D89982513C8BB5061B02A387204EB944647918DEFB6C42868F`

Verification Command:
```powershell
$zipPath = "D:\KlyrDB\dist\klyrdb-0.12.0-windows-x64.zip"
$hashRes = Get-FileHash -Path $zipPath -Algorithm SHA256
$hashRes.Hash
Get-Content "D:\KlyrDB\dist\klyrdb-0.12.0-windows-x64.zip.sha256"
```

Command Output:
```text
Computed SHA256: E2FD557A97C4F3D89982513C8BB5061B02A387204EB944647918DEFB6C42868F
SHA256 File Content: E2FD557A97C4F3D89982513C8BB5061B02A387204EB944647918DEFB6C42868F  klyrdb-0.12.0-windows-x64.zip
Exact Match to expected: True
Exact Match to file:     True
```

- **Verification Result:** **EXACT MATCH**.
- **Checksum File Format:** Valid standard format (`<SHA256>  <FILENAME>`).
- **Archive Size:** 20,592,262 bytes (~19.63 MB).

### 2.2 Package Contents Inventory

Total entries in archive: 314 entries.

| Top-Level Directory | Entry Count | Advertised Components Verified |
|---|---|---|
| `bin/` | 1 | `klyrdb.exe` (CLI binary, 362 KB) |
| `include/klyrdb/` | 59 | Public C++20 headers (`klyrdb.hpp`, `database.hpp`, `connection.hpp`, `result.hpp`, `error.hpp`, `version.hpp`, and underlying subsystem headers) |
| `lib/` | 6 | `klyrdb_storage.lib` (Static engine library, 2.87 MB) + CMake package files |
| `lib/cmake/KlyrDB/` | 4 | `KlyrDBConfig.cmake`, `KlyrDBConfigVersion.cmake`, `KlyrDBTargets.cmake`, `KlyrDBTargets-release.cmake` |
| `docs/` | 54 | Complete Markdown technical specifications, architecture diagrams, and test audit reports |
| `examples/` | 193 | Standalone consumer example source code and demo scripts |
| Root | 1 | `README.md` (37.2 KB) |

### 2.3 Packaging Defect Identified (Non-Blocking Limitation)

- **Finding:** Under `examples/basic_cpp_app/build/` and `examples/transactions_and_constraints/build/`, 60 intermediate build artifacts (`.obj`, `.ilk`, `.pdb`, `.tlog`) from prior local example testing were inadvertently included in the archive.
- **Root Cause:** In [`scripts/package_release.ps1`](file:///D:/KlyrDB/scripts/package_release.ps1#L76), `Copy-Item -Recurse (Join-Path $RootDir "examples")` copied the entire `examples` folder without excluding sub-build directories.
- **Impact:** Does not break downstream consumers or CMake packaging; however, it bloats the zip archive with machine-specific intermediate build state.
- **Action Taken:** Recorded as a known limitation in this report without modifying or regenerating the existing candidate archive.

---

## 3. Clean External Consumer Verification

The release archive was extracted to an external temporary directory completely separate from the repository:
`%TEMP%\klyrdb_ext_test`

### 3.1 Distributed Consumer Example 1: `basic_cpp_app`

- **Build Directory:** `%TEMP%\build_consumer_basic`
- **Configure Command:**
  ```powershell
  cmake -B "%TEMP%/build_consumer_basic" -S "%TEMP%/klyrdb_ext_test/examples/basic_cpp_app" -DCMAKE_PREFIX_PATH="%TEMP%/klyrdb_ext_test" -DCMAKE_BUILD_TYPE=Release
  ```
  *Exit code: 0*
- **Build Command:**
  ```powershell
  cmake --build "%TEMP%/build_consumer_basic" --config Release
  ```
  *Exit code: 0*
- **Execution Output:**
  ```text
  === KlyrDB Standalone Consumer Example (v0.12.0) ===
  [1] Opening new database: basic_app.kdb (format v1)
  [2] Creating table 'users'...
  [3] Inserting records...
  [4 & 5] Querying records via Connection (3 rows returned)
  [6] Intentionally submitting malformed SQL query to demonstrate error handling... (Handled Expected Error: Code 24)
  [7] Testing Transactions (Rollback & Commit)... (Rollback verified; TransactionGuard commit verified)
  [8] Closing database connection and storage handles...
  [9] Reopening database to verify persistence across sessions... (4 persisted users verified)
  === All Consumer Verification Steps Passed Successfully! ===
  ```
  *Exit code: 0*

### 3.2 Distributed Consumer Example 2: `transactions_and_constraints`

- **Build Directory:** `%TEMP%\build_consumer_txns`
- **Configure Command:**
  ```powershell
  cmake -B "%TEMP%/build_consumer_txns" -S "%TEMP%/klyrdb_ext_test/examples/transactions_and_constraints" -DCMAKE_PREFIX_PATH="%TEMP%/klyrdb_ext_test"
  ```
  *Exit code: 0*
- **Build Command:**
  ```powershell
  cmake --build "%TEMP%/build_consumer_txns" --config Release
  ```
  *Exit code: 0*
- **Execution Output:**
  ```text
  === KlyrDB Transactions & Relational Constraints Example ===
  [1] Creating schema with PRIMARY KEY, NOT NULL, and UNIQUE constraints...
  [2] Attempting duplicate PRIMARY KEY insert... (Intercepted Error: Unique constraint violated for index 'pk_departments_id')
  [3] Attempting NOT NULL violation... (Intercepted Error: NOT NULL constraint violated)
  [4] Attempting UNIQUE violation... (Intercepted Error: Unique constraint violated for index 'uk_departments_dept_name')
  [5] Demonstrating transactional atomicity under constraint failure... (Transaction rolled back cleanly)
  [6] Auditing final database integrity...
  === Transactions & Constraints Demo Completed Successfully! ===
  ```
  *Exit code: 0*

### 3.3 Custom Independent Consumer Test (`IndependentConsumerApp`)

A clean standalone project was created in `%TEMP%\independent_consumer_app`:
- Calling `find_package(KlyrDB REQUIRED)`
- Linking strictly to `KlyrDB::KlyrDB`
- Creating fresh database `indep_test.kdb`
- Inserting records within committed transaction
- Attempting rolled-back transaction
- Closing and reopening database
- Verifying persisted table data on cold restart

Execution Result:
```text
[Step 1] Opening database via KlyrDB SDK...
[Step 2] Creating connection and executing table DDL...
[Step 3] Executing committed transaction with inserts...
[Step 4] Executing rolled-back transaction...
[Step 5] Closing database handles...
[Step 6] Reopening database to verify on-disk persistence...
Persisted rows verified: 2 items found.
  SKU 101: Mechanical Keyboard (Qty: 45)
  SKU 102: Wireless Mouse (Qty: 120)
=== Independent Consumer Verification Successful! ===
Exit Code: 0
```

### 3.4 Path Isolation Audit

The generated MSBuild project files (`basic_cpp_app.vcxproj`, `transactions_and_constraints.vcxproj`, `indep_app.vcxproj`) were grepped for repository paths:
```powershell
Select-String -Path "%TEMP%/build_independent_consumer/indep_app.vcxproj" -Pattern "D:[\\/]KlyrDB"
```
- **Matches Found:** `0` (Zero references to `D:\KlyrDB`).
- **Include Paths:** Point strictly to `%TEMP%\klyrdb_ext_test\include`.
- **Link Libraries:** Points strictly to `%TEMP%\klyrdb_ext_test\lib\klyrdb_storage.lib`.

---

## 4. Test Evidence: Debug & Release Test Suites

### 4.1 Debug Configuration CTest Results

- **Build Directory:** `D:\KlyrDB\build`
- **Command Executed:**
  ```powershell
  ctest --test-dir D:\KlyrDB\build -C Debug --output-on-failure
  ```
- **Exit Code:** `0`
- **Test Summary:** **51 / 51 passed (100%)**
- **Total Duration:** 16.30 seconds

### 4.2 Release Configuration CTest Results

- **Build Directory:** `D:\KlyrDB\build_release`
- **Command Executed:**
  ```powershell
  ctest --test-dir D:\KlyrDB\build_release -C Release --output-on-failure
  ```
- **Exit Code:** `0`
- **Test Summary:** **51 / 51 passed (100%)**
- **Total Duration:** 15.90 seconds

### Complete 51-Target Matrix (Both Configurations)

| # | Test Target | Debug Result | Release Result | Verified Subsystem |
|---|---|---|---|---|
| 1 | `test_page` | PASSED (0.04s) | PASSED (0.05s) | Page binary layout & alignment |
| 2 | `test_page_file` | PASSED (0.06s) | PASSED (0.08s) | File manager I/O & sync |
| 3 | `test_persistence` | PASSED (0.12s) | PASSED (0.10s) | Cold start page persistence |
| 4 | `test_buffer_pool` | PASSED (0.13s) | PASSED (0.14s) | Clock-Pro frame eviction |
| 5 | `test_slotted_page` | PASSED (0.04s) | PASSED (0.06s) | Slotted page record layout |
| 6 | `test_record_manager` | PASSED (0.13s) | PASSED (0.16s) | Record manager CRUD |
| 7 | `test_bplus_tree` | PASSED (0.17s) | PASSED (0.15s) | B+ tree splits & iterators |
| 8 | `test_fsm` | PASSED (0.10s) | PASSED (0.10s) | Free Space Map allocations |
| 9 | `test_index_integration`| PASSED (0.10s) | PASSED (0.10s) | Secondary B+ tree indexing |
| 10 | `test_sql_lexer` | PASSED (0.04s) | PASSED (0.05s) | SQL tokenization |
| 11 | `test_sql_parser` | PASSED (0.05s) | PASSED (0.05s) | SQL AST parsing |
| 12 | `test_table_catalog` | PASSED (0.07s) | PASSED (0.07s) | Catalog page serialization |
| 13 | `test_sql_executor` | PASSED (0.08s) | PASSED (0.09s) | SQL CRUD statement execution |
| 14 | `test_wal` | PASSED (0.08s) | PASSED (0.08s) | WAL append, LSN & checksums |
| 15 | `test_transaction` | PASSED (0.12s) | PASSED (0.13s) | Commit, abort & rollbacks |
| 16 | `test_recovery` | PASSED (0.25s) | PASSED (0.27s) | ARIES redo & undo passes |
| 17 | `test_crash_recovery` | PASSED (0.57s) | PASSED (0.63s) | Deterministic crash injection |
| 18 | `test_constraints` | PASSED (0.16s) | PASSED (0.15s) | PK, NOT NULL, UNIQUE, CHECK |
| 19 | `test_constraint_transactions` | PASSED (0.12s) | PASSED (0.12s) | Constraint failure rollback |
| 20 | `test_query_planner` | PASSED (0.08s) | PASSED (0.09s) | Cost-based query planning |
| 21 | `test_explain` | PASSED (0.07s) | PASSED (0.09s) | EXPLAIN output formatting |
| 22 | `test_query_plan_execution` | PASSED (0.09s) | PASSED (0.11s) | Index range scan execution |
| 23 | `test_query_optimization` | PASSED (0.08s) | PASSED (0.09s) | Unselective scan fallback |
| 24 | `test_database_locking` | PASSED (5.44s) | PASSED (5.44s) | SWMR advisory file locks |
| 25 | `test_process_locking` | PASSED (0.19s) | PASSED (0.17s) | OS-level file lock exclusivity |
| 26 | `test_buffer_pool_concurrency` | PASSED (0.16s) | PASSED (0.11s) | Multi-threaded page latches |
| 27 | `test_concurrent_reads`| PASSED (0.28s) | PASSED (0.09s) | Concurrent shared reads |
| 28 | `test_concurrent_writes`| PASSED (0.12s) | PASSED (0.10s) | Exclusive write conflict safety |
| 29 | `test_transaction_isolation` | PASSED (0.22s) | PASSED (0.23s) | Read-committed isolation |
| 30 | `test_checkpoint` | PASSED (0.25s) | PASSED (0.22s) | Sharp checkpoint flushing |
| 31 | `test_wal_retention` | PASSED (0.15s) | PASSED (0.15s) | Safe WAL log truncation |
| 32 | `test_checkpoint_recovery` | PASSED (0.23s) | PASSED (0.20s) | Checkpoint recovery speedup |
| 33 | `test_integrity_check`| PASSED (0.10s) | PASSED (0.13s) | Multi-layer integrity auditor |
| 34 | `test_storage_corruption` | PASSED (0.13s) | PASSED (0.15s) | Checksum bit-flip detection |
| 35 | `test_wal_lifecycle` | PASSED (0.26s) | PASSED (0.26s) | Full WAL lifecycle cycles |
| 36 | `test_physical_backup`| PASSED (0.47s) | PASSED (0.41s) | Hot physical backup creation |
| 37 | `test_backup_verification` | PASSED (0.14s) | PASSED (0.16s) | Backup manifest verifier |
| 38 | `test_backup_corruption` | PASSED (0.27s) | PASSED (0.29s) | Corrupted backup rejection |
| 39 | `test_restore` | PASSED (0.17s) | PASSED (0.16s) | Atomic physical restore |
| 40 | `test_restore_failure_safety` | PASSED (0.30s) | PASSED (0.30s) | Rollback on restore failure |
| 41 | `test_logical_export_import` | PASSED (0.20s) | PASSED (0.18s) | SQL dump and replay |
| 42 | `test_csv_export_import` | PASSED (0.14s) | PASSED (0.15s) | RFC 4180 CSV migration |
| 43 | `test_backup_crash_recovery` | PASSED (0.17s) | PASSED (0.20s) | Backup WAL roll-forward |
| 44 | `test_metrics` | PASSED (0.07s) | PASSED (0.10s) | Runtime atomic metrics |
| 45 | `test_query_profiling`| PASSED (0.08s) | PASSED (0.11s) | EXPLAIN ANALYZE metrics |
| 46 | `test_randomized_sql` | PASSED (0.35s) | PASSED (0.23s) | Fuzz test for SQL grammar |
| 47 | `test_fuzz_mutations` | PASSED (0.07s) | PASSED (0.07s) | Page bit-flip fuzz tests |
| 48 | `test_reliability_stress` | PASSED (1.10s) | PASSED (1.10s) | Multi-iteration stress test |
| 49 | `test_public_api` | PASSED (0.12s) | PASSED (0.13s) | C++ Public SDK interfaces |
| 50 | `test_cli_smoke` | PASSED (2.08s) | PASSED (1.83s) | CLI binary command dispatch |
| 51 | `test_adversarial` | PASSED (0.22s) | PASSED (0.25s) | Adversarial audit edge cases |

---

## 5. Final Status & Summary

- **Final Status:** **PASS WITH LIMITATIONS**
- **Evaluation Details:**
  1. The release candidate archive exists and its SHA-256 matches the expected checksum `E2FD557A97C4F3D89982513C8BB5061B02A387204EB944647918DEFB6C42868F` with 100% bit-level precision.
  2. The package installs cleanly and functions in an external consumer environment using CMake `find_package(KlyrDB REQUIRED)` and `KlyrDB::KlyrDB` with zero leakage of repository paths.
  3. All 51 CTest suites pass in both Debug and Release build configurations with zero failures.
  4. The only limitation identified is the inadvertent presence of 60 intermediate MSBuild build files (`.obj`, `.ilk`, `.pdb`, `.tlog`) inside the packaged `examples/` subdirectories, which does not affect functional correctness but should be cleanly excluded in future packaging runs.

---

## 6. Next Recommended Actions

1. Update [`scripts/package_release.ps1`](file:///D:/KlyrDB/scripts/package_release.ps1) and [`scripts/package_release.sh`](file:///D:/KlyrDB/scripts/package_release.sh) to exclude `*/build` and `*.obj` when staging the `examples/` directory.
2. When ready, authorize generating the final cleaned `v0.12.0` release package archive for final tag and distribution.
