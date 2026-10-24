# KlyrDB v0.12.0 — Release Artifact Verification & Audit Report

**Date of Audit:** 2026-10-24  
**Project:** KlyrDB C++20 Database Engine  
**Release Candidate Version:** `v0.12.0`  
**Distribution Archive:** `D:\KlyrDB\dist\klyrdb-0.12.0-windows-x64.zip`  
**Expected Checksum File:** `D:\KlyrDB\dist\klyrdb-0.12.0-windows-x64.zip.sha256`  
**Repository:** `https://github.com/mnp210/Klyr-DB`  
**Evaluator:** Senior C++ Release Engineer  
**Final Verdict:** **PASS**

---

## 1. Actual Artifact Properties & Checksum Verification

The release distribution archive and its checksum companion file were inspected on local storage:

| Property | Archive File (`.zip`) | Checksum File (`.sha256`) |
|---|---|---|
| **Path** | `D:\KlyrDB\dist\klyrdb-0.12.0-windows-x64.zip` | `D:\KlyrDB\dist\klyrdb-0.12.0-windows-x64.zip.sha256` |
| **File Exists** | `True` | `True` |
| **Byte Size** | `4,751,052 bytes` (~4.53 MB) | `100 bytes` |
| **Last Modification (UTC)** | `2026-10-24T15:11:55.2434039Z` | `2026-10-24T15:11:55.3162072Z` |
| **Checksum File Contents** | `58CF6568E9FB5DF8B37A865651A5E99218EDE451F3A9523689878DD4A1242DEA  klyrdb-0.12.0-windows-x64.zip` |
| **Computed SHA-256** | `58CF6568E9FB5DF8B37A865651A5E99218EDE451F3A9523689878DD4A1242DEA` |
| **Verification Match** | **EXACT MATCH (`True`)** |

```powershell
Get-FileHash -Path D:\KlyrDB\dist\klyrdb-0.12.0-windows-x64.zip -Algorithm SHA256
# Result: 58CF6568E9FB5DF8B37A865651A5E99218EDE451F3A9523689878DD4A1242DEA
```

> [!NOTE]
> **Historical Supersession:** This verified archive supersedes the preliminary candidate archive (20,592,262 bytes, historical SHA-256: `E2FD557A97C4F3D89982513C8BB5061B02A387204EB944647918DEFB6C42868F`), which inadvertently included 60 intermediate MSBuild compiler files under `examples/**/build/`.

---

## 2. Archive Contents & Structure Audit

The archive was opened and enumerated using `System.IO.Compression.ZipFile`. All 131 entries were validated:

### 2.1 Entry Count by Category

| Directory / Category | Entry Count | Verified Components |
|---|---|---|
| **`bin/`** | 1 entry | `bin/klyrdb.exe` (Standalone CLI executable, 882,176 bytes) |
| **`lib/`** | 2 entries | `lib/klyrdb_storage.lib` (Static engine library, 27,445,016 bytes), `lib/cmake/` directory |
| **`lib/cmake/KlyrDB/`** | 4 entries | `KlyrDBConfig.cmake`, `KlyrDBConfigVersion.cmake`, `KlyrDBTargets.cmake`, `KlyrDBTargets-release.cmake` |
| **`include/klyrdb/`** | 59 entries | Master umbrella header (`klyrdb.hpp`), subsystem headers (`storage/`, `index/`, `catalog/`, `wal/`, `txn/`, `recovery/`, `sql/`, `backup/`, `diagnostics/`) |
| **`docs/`** | 56 entries | Technical specifications, format schemas, test matrices, and verification reports |
| **`examples/`** | 8 entries | Pure C++20 consumer sources and CMake project files (`basic_cpp_app`, `transactions_and_constraints`, demos) |
| **Root** | 1 entry | `README.md` |
| **Total Entries** | **131 entries** | Zero intermediate compiler files |

### 2.2 Intermediate & Unwanted Artifacts Check
An exhaustive audit scanned for unwanted build artifacts:
- Intermediate objects (`*.obj`, `*.o`): **0**
- Symbol & debug files (`*.pdb`, `*.ilk`): **0**
- Build logs & state files (`*.tlog`, `*.idb`, `*.lastbuildstate`): **0**
- Visual Studio project files (`*.vcxproj*`, `*.sln*`): **0**
- Nested build directories (`build/`): **0**
- Temporary database files (`*.kdb`, `*.wal`, `*.lock`, `*.tmp`, `*.bak`): **0**
- Suspicious match analysis: A single entry matching the pattern `*build*` was `docs/building.md`, which is a legitimate documentation file.

### 2.3 Runtime Dependency Analysis
Binary dependencies of `bin/klyrdb.exe` were inspected via `dumpbin /dependents`:
- `KERNEL32.dll` (Windows Operating System Core API)
- `MSVCP140.dll` & `VCRUNTIME140.dll` / `VCRUNTIME140_1.dll` (Standard Microsoft C++ Runtime)
- `api-ms-win-crt-*` (Universal C Runtime, Windows 10/11 built-in)
- **Third-Party External DLLs:** **None (0)**. The binary is fully self-contained.

---

## 3. Extracted Distribution & Consumer Verification

The archive was extracted to an isolated temporary location completely outside `D:\KlyrDB`:  
`C:\Users\lnami\AppData\Local\Temp\klyrdb_artifact_audit_test`

### 3.1 Supplied Consumer 1: `basic_cpp_app`
- **Location:** `examples/basic_cpp_app`
- **Configuration Command:**
  ```powershell
  cmake -B build -S . -DCMAKE_PREFIX_PATH="C:/Users/lnami/AppData/Local/Temp/klyrdb_artifact_audit_test" -DCMAKE_BUILD_TYPE=Release
  ```
- **Exit Codes:** Configure: `0` | Build: `0` | Execution: `0`
- **Verification Evidence:**
  - Database creation and table DDL (`CREATE TABLE users ...`).
  - Insertion of initial records.
  - Query execution (`SELECT`) and row iteration via `klyrdb::Connection`.
  - Intentional malformed SQL error interception.
  - Transaction rollback demonstration (uncommitted row rolled back cleanly).
  - Transaction commit via RAII `TransactionGuard`.
  - Cross-session persistence across database close and reopen (4 persisted rows).

### 3.2 Supplied Consumer 2: `transactions_and_constraints`
- **Location:** `examples/transactions_and_constraints`
- **Configuration Command:**
  ```powershell
  cmake -B build -S . -DCMAKE_PREFIX_PATH="C:/Users/lnami/AppData/Local/Temp/klyrdb_artifact_audit_test" -DCMAKE_BUILD_TYPE=Release
  ```
- **Exit Codes:** Configure: `0` | Build: `0` | Execution: `0`
- **Verification Evidence:**
  - Table creation with `PRIMARY KEY`, `NOT NULL`, and `UNIQUE` constraints.
  - Rejection of duplicate `PRIMARY KEY` (code 20).
  - Rejection of `NOT NULL` constraint violation (code 18).
  - Rejection of duplicate `UNIQUE` constraint key (code 20).
  - Multi-row atomicity under constraint failure with automatic rollback via `TransactionGuard`.
  - Storage integrity audit reporting 0 errors and 0 warnings.

### 3.3 Independent Standalone Consumer: `audit_standalone_app`
An out-of-tree consumer application was created in a dedicated directory:
```cmake
cmake_minimum_required(VERSION 3.20)
project(audit_standalone_app LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(KlyrDB REQUIRED)
add_executable(audit_standalone_app main.cpp)
target_link_libraries(audit_standalone_app PRIVATE KlyrDB::KlyrDB)
```

- **Exit Codes:** Configure: `0` | Build: `0` | Execution: `0`
- **Runtime Verification:**
  - Opened new database `consumer_audit.kdb`.
  - Executed DDL creating table `metrics`.
  - Transaction committed 2 records (`latency_p50`, `latency_p95`).
  - Transaction rollback executed on scope exit without commit; verified row count for rolled-back anomaly is 0.
  - Closed database and reopened from disk; verified exactly 2 committed records persisted.
- **Path Isolation Audit:**
  - Inspected all 25 compiler and linker `.tlog` files (`CL.command.1.tlog`, `link.command.1.tlog`).
  - Linkage resolved strictly against `C:\Users\lnami\AppData\Local\Temp\klyrdb_artifact_audit_test\lib\klyrdb_storage.lib` and header include path `.../include`.
  - References to source tree `D:\KlyrDB`: **0 (Zero)**.

---

## 4. Known Engine Boundaries & Limitations

1. **Concurrency:** Single-Writer / Multiple-Reader (SWMR) concurrency model with advisory OS process file locks (`.lock`). Write transactions serialize under an exclusive table/database lock.
2. **Record Size:** Slotted-page maximum single-tuple payload of 4,072 bytes (records spanning multiple pages / overflow pages are not supported).
3. **SQL Features:** Supports `SELECT`, `INSERT`, `UPDATE`, `DELETE`, `WHERE`, `ORDER BY`, `LIMIT`. No `JOIN` operators, subqueries, `GROUP BY`, `HAVING`, window functions, or SQL aggregate functions (`COUNT(*)`, `SUM()`).
4. **Deployment:** Embedded library (`KlyrDB::KlyrDB`) and CLI utility (`klyrdb.exe`) only. No background network daemon or wire protocol.
5. **Operating System Distribution:** Pre-built distribution package is compiled for Windows x64. Non-Windows environments build from source using CMake presets.

---

## 5. Final Audit Verdict

| Audit Check | Requirement | Result |
|---|---|---|
| **Artifact Existence & Integrity** | ZIP and SHA-256 files present, hashes match | **PASS** (`58CF6568...`) |
| **Archive Hygiene** | 0 intermediate build files, 0 object files, 0 PDBs | **PASS** (131 clean entries) |
| **Runtime Dependencies** | Self-contained, standard Windows / Universal CRT | **PASS** (0 third-party DLLs) |
| **Supplied Example 1** | Build & run `basic_cpp_app` | **PASS** (Exit code 0) |
| **Supplied Example 2** | Build & run `transactions_and_constraints` | **PASS** (Exit code 0) |
| **Standalone Consumer** | External `find_package(KlyrDB REQUIRED)` app | **PASS** (Exit code 0) |
| **Path Isolation** | Zero dependency or leakage to `D:\KlyrDB` | **PASS** (0 references) |
| **Documentation Alignment** | Checksums and sizes accurate across docs | **PASS** |

**Final Status:** **PASS**  
The release candidate package `dist/klyrdb-0.12.0-windows-x64.zip` is completely verified, free of intermediate artifacts, and ready for distribution.
