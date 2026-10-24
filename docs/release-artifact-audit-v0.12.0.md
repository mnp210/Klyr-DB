# KlyrDB v0.12.0 — Release Artifact Verification & Audit Report

**Date of Audit:** 2026-10-24  
**Project:** KlyrDB C++20 Database Engine  
**Release Candidate Version:** `v0.12.0`  
**Distribution Archive:** `D:\KlyrDB\dist\klyrdb-0.12.0-windows-x64.zip`  
**Expected Checksum File:** `D:\KlyrDB\dist\klyrdb-0.12.0-windows-x64.zip.sha256`  
**Expected SHA-256:** `58CF6568E9FB5DF8B37A865651A5E99218EDE451F3A9523689878DD4A1242DEA`  
**Repository:** `https://github.com/mnp210/Klyr-DB`  
**Evaluator:** Senior C++ Release Engineer & Systems Auditor  
**Final Verdict:** **PASS**

---

## 1. Physical Artifact Properties & Checksum Verification

The release distribution package and companion checksum file were audited on local storage:

| Property | Archive File (`.zip`) | Checksum Companion File (`.sha256`) |
|---|---|---|
| **Local Path** | `D:\KlyrDB\dist\klyrdb-0.12.0-windows-x64.zip` | `D:\KlyrDB\dist\klyrdb-0.12.0-windows-x64.zip.sha256` |
| **Physical Existence** | `True` | `True` |
| **On-Disk Byte Size** | `4,751,052 bytes` (4.53 MB) | `100 bytes` |
| **File Timestamps (UTC)** | `2026-10-24T15:11:55.2434039Z` | `2026-10-24T15:11:55.3162072Z` |
| **Checksum File Contents** | `58CF6568E9FB5DF8B37A865651A5E99218EDE451F3A9523689878DD4A1242DEA  klyrdb-0.12.0-windows-x64.zip` |
| **Computed SHA-256 (PowerShell)** | `58CF6568E9FB5DF8B37A865651A5E99218EDE451F3A9523689878DD4A1242DEA` |
| **Verification Match** | **EXACT MATCH (`True`)** |

### 1.1 PowerShell Recomputation Evidence
```powershell
Get-FileHash -Path 'D:\KlyrDB\dist\klyrdb-0.12.0-windows-x64.zip' -Algorithm SHA256

# Algorithm : SHA256
# Hash      : 58CF6568E9FB5DF8B37A865651A5E99218EDE451F3A9523689878DD4A1242DEA
# Path      : D:\KlyrDB\dist\klyrdb-0.12.0-windows-x64.zip
```

> [!NOTE]
> **Historical Supersession Notice:** This verified archive (`4,751,052 bytes`, SHA-256: `58CF6568...`) supersedes the preliminary candidate archive (`20,592,262 bytes`, historical SHA-256: `E2FD557A97C4F3D89982513C8BB5061B02A387204EB944647918DEFB6C42868F`), which was rebuilt in Phase 13B after discovering 60 intermediate MSBuild compiler files in `examples/**/build/`.

---

## 2. Package Inventory & Hygiene Audit

The ZIP archive was opened and enumerated using `zipfile` and `System.IO.Compression.ZipFile`. All 131 entries were verified against explicit categorization rules.

### 2.1 Archive Metrics Summary
- **Total ZIP Entries:** 131 entries
  - **Directory Markers:** 1 entry (`bin/`)
  - **File Entries:** 130 files
- **Total Uncompressed Size:** `28,882,601 bytes` (27.54 MB)
- **Total Compressed Size of Entries:** `4,733,232 bytes` (4.51 MB)
- **Archive Size on Disk:** `4,751,052 bytes` (difference of 17,820 bytes accounts for local file headers, central directory records, and end of central directory record)
- **Overall Compression Ratio:** `16.39%` (compressed size / uncompressed size)

### 2.2 Detailed Inventory Breakdown by Category

| Category / Directory | Entries | Uncompressed Size | Compressed Size | Key Files & Descriptions |
|---|---|---|---|---|
| **CLI Binary (`bin/`)** | 1 file | 882,176 bytes | 395,490 bytes | `bin/klyrdb.exe` (Standalone CLI tool) |
| **Static Library (`lib/`)** | 1 file | 27,445,016 bytes | 4,134,952 bytes | `lib/klyrdb_storage.lib` (MSVC C++20 static engine) |
| **CMake Config (`lib/cmake/`)** | 4 files | 9,933 bytes | 2,752 bytes | `KlyrDBConfig.cmake`, `KlyrDBConfigVersion.cmake`, `KlyrDBTargets.cmake`, `KlyrDBTargets-release.cmake` |
| **Public Headers (`include/`)** | 59 files | 224,534 bytes | 49,678 bytes | Umbrella header `<klyrdb/klyrdb.hpp>` and 58 modular subsystem headers |
| **Documentation (`docs/`, root)** | 57 files | 305,256 bytes | 98,162 bytes | `README.md` (root) and 56 subsystem specs, manuals, and reports in `docs/` |
| **Examples (`examples/`)** | 8 files | 15,686 bytes | 5,236 bytes | `basic_cpp_app`, `transactions_and_constraints`, `storage_demo.cpp`, `sql_demo.cpp`, `wal_demo.cpp`, `constraints_demo.cpp` |
| **Directory Marker** | 1 dir | 0 bytes | 0 bytes | `bin/` |
| **Total** | **131 entries** | **28,882,601 bytes** | **4,733,232 bytes** | **Fully reconciled package** |

### 2.3 Artifact Hygiene & Intermediate Checks
A comprehensive scan checked for unwanted compiler or temporary files:
- Intermediate object files (`*.obj`, `*.o`): **0**
- Debug symbol files (`*.pdb`, `*.idb`): **0**
- Linker temporary files (`*.ilk`, `*.exp`): **0**
- Build logs & state caches (`*.tlog`, `*.lastbuildstate`): **0**
- IDE project files (`*.vcxproj*`, `*.sln`): **0**
- Temporary database & log files (`*.kdb`, `*.wal`, `*.lock`, `*.tmp`): **0**
- Forbidden / intermediate files found: **0 (Zero)**

### 2.4 Binary Runtime Dependency Analysis
Binary dependencies of `bin/klyrdb.exe` were extracted from its PE header (x86_64, Machine `0x8664`, PE32+):
- `KERNEL32.dll` (Windows Operating System Core API: file I/O, `LockFileEx`, heap management)
- `VCRUNTIME140.dll` (MSVC C runtime base)
- `VCRUNTIME140_1.dll` (MSVC x64 exception handling runtime)
- `MSVCP140.dll` (MSVC standard C++ library)
- **Third-Party External DLLs:** **None (0)**.

> [!IMPORTANT]
> **Runtime Compatibility Notice:** While KlyrDB packages zero third-party dynamic libraries, target Windows environments require the standard Microsoft Visual C++ 2015–2022 Redistributable (x64) or Universal C Runtime (present by default on modern Windows 10/11). We do not claim universal compatibility across stripped or bare Windows environments without the VC runtime.

---

## 3. Extracted Distribution & Consumer Verification

The distribution archive was extracted to an isolated temporary directory outside the repository:  
`C:\Users\lnami\AppData\Local\Temp\klyrdb_pkg_verify_20261024\pkg`

Three distinct consumer projects were built and executed against the extracted package using only `-DCMAKE_PREFIX_PATH`:

### 3.1 Supplied Consumer 1: `basic_cpp_app`
- **Source:** `pkg/examples/basic_cpp_app`
- **Build Path:** `build_basic/`
- **CMake Configure Command:**
  ```powershell
  cmake -B build_basic -S pkg/examples/basic_cpp_app -DCMAKE_PREFIX_PATH=".../pkg"
  ```
  *Exit code: `0`*
- **CMake Build Command:**
  ```powershell
  cmake --build build_basic --config Release
  ```
  *Exit code: `0`*
- **Runtime Execution:**
  ```text
  === KlyrDB Standalone Consumer Example (v0.12.0) ===
  [1] Opening new database: basic_app.kdb (format v1)
  [2] Creating table 'users'...
  [3] Inserting records... (Inserted 3 initial records)
  [4 & 5] Querying records via Connection (Rows returned: 3)
  [6] Intentionally submitting malformed SQL query... [Handled Expected Error Code 24]
  [7] Testing Transactions (Rollback & Commit):
      - Beginning transaction for uncommitted insert (id=99)...
      - Executing ROLLBACK...
      - Records matching id=99 post-rollback: 0 (Expected: 0)
      - Using RAII TransactionGuard to commit new user (id=4)... (Committed successfully)
  [8] Closing database connection...
  [9] Reopening database to verify persistence across sessions...
      Reopened database contains 4 persisted users.
  === All Consumer Verification Steps Passed Successfully! ===
  ```
  *Exit code: `0`*

### 3.2 Supplied Consumer 2: `transactions_and_constraints`
- **Source:** `pkg/examples/transactions_and_constraints`
- **Build Path:** `build_constraints/`
- **CMake Configure & Build:**
  - Configure exit code: `0`
  - Build exit code: `0`
- **Runtime Execution:**
  - Tested schema creation with `PRIMARY KEY`, `NOT NULL`, and `UNIQUE` constraints.
  - Successfully intercepted duplicate PK error.
  - Successfully intercepted `NOT NULL` constraint violation.
  - Successfully intercepted `UNIQUE` constraint violation.
  - Verified `TransactionGuard` automatic rollback on scope exit under constraint failure (uncommitted row rolled back cleanly; 0 residual rows).
  - Storage integrity audit confirmed database is 100% healthy.
  *Exit code: `0`*

### 3.3 Independent Standalone Consumer: `external_consumer`
An independent out-of-tree consumer application was created from scratch in `external_consumer/`:
```cmake
cmake_minimum_required(VERSION 3.20)
project(independent_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(KlyrDB REQUIRED)
add_executable(independent_consumer main.cpp)
target_link_libraries(independent_consumer PRIVATE KlyrDB::KlyrDB)
```

- **Configuration & Build:**
  - `cmake -B build_external -S external_consumer -DCMAKE_PREFIX_PATH=".../pkg"` -> Exit code: `0`
  - `cmake --build build_external --config Release` -> Exit code: `0`
- **Execution Output:**
  ```text
  [Consumer] KlyrDB Independent Verification Consumer running...
  [Consumer] Version: 0.12.0
  [Consumer] Storage Format: v1
  [Consumer] Transaction rolling back uncommitted order 3...
  [Consumer] Rollback verified: order 3 absent.
  [Consumer] Commit verified: order 3 committed.
  [Consumer] Persisted rows in reopened database:
    - Order #1: Server Rack (Qty: 2)
    - Order #2: Patch Cable (Qty: 24)
    - Order #3: Confirmed Item (Qty: 5)
  [Consumer] Independent consumer verification PASSED 100%.
  ```
  *Exit code: `0`*

### 3.4 Build Isolation Audit
All generated project files, build logs, and `.tlog` files in `build_external/` were scanned for references to development directories:
- References to `D:\KlyrDB\src`: **0**
- References to `D:\KlyrDB\include`: **0**
- References to `D:\KlyrDB\build`: **0**
- **Conclusion:** The consumer built and linked exclusively against the exported artifacts in the isolated package folder.

---

## 4. Documentation Reconciliation & Audit Trail

All six documentation files referencing the release package were audited for consistency:

1. [`README.md`](README.md):
   - Archive path: `dist/klyrdb-0.12.0-windows-x64.zip`
   - Checksum: `58CF6568E9FB5DF8B37A865651A5E99218EDE451F3A9523689878DD4A1242DEA` (**Matches actual**)
   - Cross-links all verification and technical audit reports.
2. [`docs/release-notes-v0.12.0.md`](docs/release-notes-v0.12.0.md):
   - Checksum: `58CF6568E9FB5DF8B37A865651A5E99218EDE451F3A9523689878DD4A1242DEA` (**Matches actual**)
   - File size: `4,751,052 bytes` (**Matches actual**)
   - Commit baseline: `ab34daa` (records clean repackaging baseline).
3. [`docs/phase-13a-release-verification.md`](docs/phase-13a-release-verification.md):
   - Retained as the historical audit documenting the discovery of the initial packaging defect (20,592,262 bytes, historical SHA-256 `E2FD557A...`).
4. [`docs/phase-13b-release-verification.md`](docs/phase-13b-release-verification.md):
   - Retained as the verification report for the clean repackaging and the first verification of `58CF6568...`.
5. [`docs/release-inventory-reconciliation.md`](docs/release-inventory-reconciliation.md):
   - Reconciles narrative counts against actual binary metrics (131 entries, 1 directory, 130 files, 28,882,601 uncompressed bytes).
6. [`docs/technical-audit-v0.12.0.md`](docs/technical-audit-v0.12.0.md):
   - Records the 51/51 CTest regression test suites and deep subsystem audit results.

---

## 5. Audit Check Matrix & Final Verdict

| Verification Item | Requirement | Measured / Observed Result | Status |
|---|---|---|---|
| **Archive File Existence** | `dist/klyrdb-0.12.0-windows-x64.zip` | Exists, 4,751,052 bytes | **PASS** |
| **Checksum File Existence** | `dist/klyrdb-0.12.0-windows-x64.zip.sha256` | Exists, 100 bytes | **PASS** |
| **SHA-256 Recomputation** | Matches `58CF6568...` | Exact match via PowerShell & Python | **PASS** |
| **Archive Hygiene** | 0 intermediate or debug files | 0 intermediate files out of 131 entries | **PASS** |
| **Static Library** | `lib/klyrdb_storage.lib` | Present, 27,445,016 bytes | **PASS** |
| **CLI Executable** | `bin/klyrdb.exe` | Present, 882,176 bytes | **PASS** |
| **CMake Export Configuration** | `lib/cmake/KlyrDB/` | 4 config files exporting `KlyrDB::KlyrDB` | **PASS** |
| **Public Headers** | `include/klyrdb/` | 59 headers with unified `<klyrdb/klyrdb.hpp>` | **PASS** |
| **Example 1 Execution** | `basic_cpp_app` | Exit code 0, CRUD + Txn persistence | **PASS** |
| **Example 2 Execution** | `transactions_and_constraints` | Exit code 0, constraint enforcement | **PASS** |
| **Independent Consumer** | `external_consumer` | Exit code 0, `find_package` resolution | **PASS** |
| **Path Isolation** | 0 references to dev tree | 0 references to `D:\KlyrDB` | **PASS** |

**Final Verdict:** **PASS**  
The release distribution package `dist/klyrdb-0.12.0-windows-x64.zip` is completely verified, relocatable, and confirmed ready for distribution.
