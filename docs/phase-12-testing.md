# KlyrDB Phase 12 — Release Engineering & SDK Testing Report

## 1. Executive Summary

- **Phase**: Phase 12 — Release Engineering, Stable C++ API, Packaging & Developer SDK
- **Version**: `0.12.0` (Storage Format v1, Backup Format v1)
- **Baseline Test Targets (Phases 1–11)**: 48 test targets
- **New Phase 12 Test Targets**:
  - `test_public_api`: Exercises the new stable public C++ API (`Database`, `Connection`, `ResultSet`, `Row`, `Value`, `TransactionGuard`, `version()`)
  - `test_cli_smoke`: Exercises all CLI commands (`version`, `help`, `create`, `info`, `sql`, `explain`, `checkpoint`, `integrity-check`, `backup`, `restore`, `dump`, `import`, `export-csv`, `import-csv`)
- **Total Test Suites**: **50 / 50 passed (100% pass rate)**
- **Release Package**: Generated and verified at `dist/klyrdb-0.12.0-windows-x64.zip` with SHA-256 checksum.

---

## 2. Toolchains & Environment Verified

| Component | Verified Specification |
|:---|:---|
| **Operating System** | Windows 11 x64 (Build 26300) |
| **Compiler** | MSVC 19.51.36260.0 (Visual Studio 2026 / Build Tools) |
| **Language Standard** | Standard ISO C++20 (`/std:c++20`, `/permissive-`, `/W4`, `/FS`) |
| **Build Generator** | CMake 3.20.0 (Visual Studio 18 2026 generator) |
| **Dependencies** | Zero external libraries (100% standard library) |

---

## 3. Test Matrix Execution

Both Debug and Release configurations were executed against all 50 CTest targets:

```text
Test project D:/KlyrDB/build
      Start  1: test_page ........................   Passed
      Start  2: test_page_file ...................   Passed
      Start  3: test_persistence .................   Passed
      Start  4: test_buffer_pool .................   Passed
      Start  5: test_slotted_page ................   Passed
      Start  6: test_record_manager ..............   Passed
      Start  7: test_bplus_tree ..................   Passed
      Start  8: test_fsm .........................   Passed
      Start  9: test_index_integration ...........   Passed
      Start 10: test_sql_lexer ...................   Passed
      Start 11: test_sql_parser ..................   Passed
      Start 12: test_table_catalog ...............   Passed
      Start 13: test_sql_executor ................   Passed
      Start 14: test_wal .........................   Passed
      Start 15: test_transaction .................   Passed
      Start 16: test_recovery ....................   Passed
      Start 17: test_crash_recovery ..............   Passed
      Start 18: test_constraints .................   Passed
      Start 19: test_constraint_transactions .....   Passed
      Start 20: test_query_planner ...............   Passed
      Start 21: test_explain .....................   Passed
      Start 22: test_query_plan_execution ........   Passed
      Start 23: test_query_optimization ..........   Passed
      Start 24: test_database_locking ............   Passed
      Start 25: test_process_locking .............   Passed
      Start 26: test_buffer_pool_concurrency .....   Passed
      Start 27: test_concurrent_reads ............   Passed
      Start 28: test_concurrent_writes ...........   Passed
      Start 29: test_transaction_isolation .......   Passed
      Start 30: test_checkpoint ..................   Passed
      Start 31: test_wal_retention ...............   Passed
      Start 32: test_checkpoint_recovery .........   Passed
      Start 33: test_integrity_check .............   Passed
      Start 34: test_storage_corruption ..........   Passed
      Start 35: test_wal_lifecycle ...............   Passed
      Start 36: test_physical_backup .............   Passed
      Start 37: test_backup_verification .........   Passed
      Start 38: test_backup_corruption ...........   Passed
      Start 39: test_restore .....................   Passed
      Start 40: test_restore_failure_safety ......   Passed
      Start 41: test_logical_export_import .......   Passed
      Start 42: test_csv_export_import ...........   Passed
      Start 43: test_backup_crash_recovery .......   Passed
      Start 44: test_metrics .....................   Passed
      Start 45: test_query_profiling .............   Passed
      Start 46: test_randomized_sql ..............   Passed
      Start 47: test_fuzz_mutations ..............   Passed
      Start 48: test_reliability_stress ..........   Passed
      Start 49: test_public_api ..................   Passed
      Start 50: test_cli_smoke ...................   Passed

100% tests passed, 0 tests failed out of 50
```

---

## 4. CMake Packaging & Installation Verification

1. **Prefix Installation**:
   - `cmake --install build --config Debug --prefix D:/KlyrDB/build/install_test`
   - Verified installation of:
     - Header tree: `include/klyrdb/klyrdb.hpp`, `database.hpp`, `connection.hpp`, etc.
     - Library: `lib/klyrdb_storage.lib`
     - Executable: `bin/klyrdb.exe`
     - CMake package files: `lib/cmake/KlyrDB/KlyrDBConfig.cmake`, `KlyrDBConfigVersion.cmake`, `KlyrDBTargets.cmake`
2. **External Consumer Verification (`examples/basic_cpp_app`)**:
   - Configured with `-DCMAKE_PREFIX_PATH="D:/KlyrDB/build/install_test"`
   - Consumed target `KlyrDB::KlyrDB` via `find_package(KlyrDB REQUIRED)`
   - Built and executed without referencing any internal database headers.
   - Tested:
     - Database creation and opening.
     - Table DDL (`CREATE TABLE users ...`).
     - Multi-row insertions.
     - `SELECT` queries with column iteration and typed value extraction.
     - Graceful error handling for non-existent tables.
     - Transaction rollback and `TransactionGuard` commit.
     - Clean closure, reopening, and data persistence verification across sessions.
3. **External Consumer Verification (`examples/transactions_and_constraints`)**:
   - Built and verified against installed package.
   - Tested:
     - Relational constraints (`PRIMARY KEY`, `NOT NULL`, `UNIQUE`).
     - Automatic rollback of transactions on constraint failure.
     - Post-error database integrity verification (`integrity_check()`).

---

## 5. Storage Compatibility Verification

- Verified opening legacy database fixture `demo_phase5.kdb` with companion `demo_phase5.kdb.wal`:
  - Format version: `1`
  - Tables: `bank_accounts` with index `idx_acc_id`
  - `klyrdb integrity-check demo_phase5.kdb`: 100% HEALTHY (0 errors, 0 warnings across all 6 pages and records).
  - Backward compatibility with format version 1 is fully preserved.

---

## 6. Release Distribution Artifacts

Generated via `scripts/package_release.ps1`:
- Archive: `dist/klyrdb-0.12.0-windows-x64.zip`
- SHA-256 Checksum: `A7B8B9348E002AAEE21FE3ABCE02C15F1981483BDDAB90DACA5202FEDF9EC3D5`
- Checksum File: `dist/klyrdb-0.12.0-windows-x64.zip.sha256`

---

## 7. Known Issues & Limitations

1. **Storage Formats**: Currently supports storage format version 1 only.
2. **Data Types**: Supports `INTEGER` (64-bit signed), `TEXT` (arbitrary UTF-8 strings), and typed `NULL`. Floating point (`REAL`) and `BLOB` are not currently in the SQL grammar.
3. **Transactions**: Single active write transaction per database instance. Multiple concurrent readers are supported via page-level latches.
4. **Networking**: Embedded in-process engine only (no client-server network protocol, similar to SQLite).
