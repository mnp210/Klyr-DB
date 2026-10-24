# KlyrDB Phase 13B — Clean Release Packaging & Final Verification Report

**Date:** 2026-10-24  
**Project:** KlyrDB C++20 Database Engine  
**Release Candidate Version:** `0.12.0`  
**Host Platform:** Windows 11 x64  
**Compiler:** Microsoft (R) C/C++ Optimizing Compiler Version 19.51.36260.0 for x64  
**CMake Version:** `4.3.1-msvc1`  
**CTest Version:** `4.3.1-msvc1`  
**Lead Evaluator:** Senior C++ Release Engineer  
**Final Verdict:** **PASS**

---

## 1. Initial State & Working Tree Audit

### 1.1 Initial Git Status & Commit Identity
Prior to making packaging adjustments, the repository working tree was inspected:

```text
Commit: ab34daa release(klyrdb): prepare v0.12.0 release candidate
Branch: main

Initial git status --short:
 M .gitignore
 M README.md
?? docs/release-notes-v0.12.0.md
```

All existing uncommitted changes in `README.md`, `.gitignore`, and `docs/release-notes-v0.12.0.md` were preserved in full without reset, cleaning, or checkout.

---

## 2. Packaging Defect Remediation

### 2.1 Problem Diagnosis
In the initial Phase 13A candidate verification, the distribution archive `dist/klyrdb-0.12.0-windows-x64.zip` (historical SHA-256: `E2FD557A97C4F3D89982513C8BB5061B02A387204EB944647918DEFB6C42868F`) was found to contain 60 intermediate MSBuild and compiler artifacts (`.obj`, `.ilk`, `.pdb`, `.tlog`, `.vcxproj`, `.slnx`) under `examples/basic_cpp_app/build/` and `examples/transactions_and_constraints/build/`. This inflated the archive size to 20,592,262 bytes (~19.63 MB) with host-specific compilation state.

The root cause was identified in the staging logic: `Copy-Item -Recurse ... examples` copied entire local development subdirectories into staging without a recursive, multi-pattern purge.

### 2.2 Script Modifications

Both [`scripts/package_release.ps1`](../scripts/package_release.ps1) and [`scripts/package_release.sh`](../scripts/package_release.sh) were updated with comprehensive exclusion filters targeting:
- Intermediate directories: `build`, `build_*`, `.vs`, `x64`, `Debug`, `Release`, `out`, `bin`, `lib`, `.cache`.
- Intermediate compiler outputs: `*.obj`, `*.ilk`, `*.pdb`, `*.tlog`, `*.idb`, `*.lastbuildstate`.
- IDE & build files: `*.vcxproj*`, `*.sln`, `*.slnx`, `*.user`, `*.suo`.
- CMake caches & metadata: `CMakeCache.txt`, `cmake_install.cmake`, `CTestTestfile.cmake`.
- Temporary databases: `*.kdb`, `*.wal`, `*.lock`, `*.tmp`, `*.bak`.

#### Diff in `scripts/package_release.ps1`:
```powershell
# Purge any build directories, compiler intermediates, CMake caches, and temporary databases
$ExcludeDirs = @("build", "build_*", ".vs", "x64", "Debug", "Release", "out", "bin", "lib", ".cache")
Get-ChildItem -Path (Join-Path $StagingDir "examples") -Recurse -Directory | Where-Object {
    $dirName = $_.Name
    foreach ($pattern in $ExcludeDirs) {
        if ($dirName -like $pattern) { return $true }
    }
    return $false
} | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue

$ExcludeExts = @("*.obj", "*.ilk", "*.pdb", "*.tlog", "*.idb", "*.lastbuildstate", "*.vcxproj*", "*.sln", "*.slnx", "*.user", "*.suo", "CMakeCache.txt", "cmake_install.cmake", "CTestTestfile.cmake", "*.kdb", "*.wal", "*.lock", "*.tmp", "*.bak")
Get-ChildItem -Path (Join-Path $StagingDir "examples") -Recurse -File | Where-Object {
    $fileName = $_.Name
    foreach ($pattern in $ExcludeExts) {
        if ($fileName -like $pattern) { return $true }
    }
    return $false
} | Remove-Item -Force -ErrorAction SilentlyContinue
```

A parallel fix was applied to `scripts/package_release.sh` using `find ... -exec rm -rf {} +`.

---

## 3. Build & Test Results

The packaging script was executed in Release mode (`powershell -ExecutionPolicy Bypass -File scripts/package_release.ps1`):

1. **CMake Configure:** Configured `build_release` in `Release` mode with `BUILD_TESTING=ON` (exit code: 0).
2. **Target Compilation:** Built all 51 test targets, CLI binary (`klyrdb.exe`), benchmarks, and static library (`klyrdb_storage.lib`) via MSVC 19.51 without compilation errors (exit code: 0).
3. **Automated CTest Suite:** Executed all 51 test suites:
   ```text
   100% tests passed, 0 tests failed out of 51
   Total Test time (real) = 15.82 sec
   ```
4. **Staging & Installation:** Cleanly staged files to `dist/klyrdb-0.12.0-windows-x64`.

---

## 4. Archive Contents & Integrity Audit

### 4.1 Archive Metrics & Checksum Comparison

| Metric | Historical Candidate (Phase 13A) | Clean Candidate (Phase 13B) | Delta / Status |
|---|---|---|---|
| **Archive File** | `dist/klyrdb-0.12.0-windows-x64.zip` | `dist/klyrdb-0.12.0-windows-x64.zip` | Overwritten |
| **SHA-256 Checksum** | `E2FD557A97C4F3D89982513C8BB5061B02A387204EB944647918DEFB6C42868F` | `58CF6568E9FB5DF8B37A865651A5E99218EDE451F3A9523689878DD4A1242DEA` | **Superseded** |
| **Archive Size** | 20,592,262 bytes (~19.63 MB) | 4,751,052 bytes (~4.53 MB) | **-76.9% reduction** |
| **Total Zip Entries** | 314 entries | 131 entries | **-183 entries** |
| **Intermediate/Build Files** | 186 entries (60 files + dirs) | **0 entries** | **Clean (0 build files)** |

### 4.2 Independent SHA-256 Hash Verification
Command executed:
```powershell
Get-FileHash D:\KlyrDB\dist\klyrdb-0.12.0-windows-x64.zip -Algorithm SHA256
Get-Content D:\KlyrDB\dist\klyrdb-0.12.0-windows-x64.zip.sha256
```
Output:
```text
Computed SHA-256: 58CF6568E9FB5DF8B37A865651A5E99218EDE451F3A9523689878DD4A1242DEA
Checksum File:    58CF6568E9FB5DF8B37A865651A5E99218EDE451F3A9523689878DD4A1242DEA  klyrdb-0.12.0-windows-x64.zip
Comparison Match: EXACT MATCH (True)
```

### 4.3 Inventory of Included Assets
A programmatic audit of `dist/klyrdb-0.12.0-windows-x64.zip` confirmed all essential artifacts are present:
- `bin/klyrdb.exe`: CLI binary (362 KB).
- `lib/klyrdb_storage.lib`: Static library (2.87 MB).
- `include/klyrdb/`: 59 public C++20 header files, including master header `klyrdb.hpp`.
- `lib/cmake/KlyrDB/`: 4 CMake integration configuration files (`KlyrDBConfig.cmake`, `KlyrDBConfigVersion.cmake`, `KlyrDBTargets.cmake`, `KlyrDBTargets-release.cmake`).
- `docs/`: 54 Markdown documentation files, specifications, and test audit records.
- `examples/`: 10 source files and example `CMakeLists.txt` files (strictly source code, zero build directories).
- `README.md`: Root project presentation and developer guide.

---

## 5. Clean External Consumer Verification

The regenerated archive was extracted to an isolated directory outside `D:\KlyrDB`:
`C:\Users\lnami\AppData\Local\Temp\klyrdb_phase13b_test`

### 5.1 Supplied Consumer 1: `basic_cpp_app`
- **Source:** Extracted `examples/basic_cpp_app`
- **Configuration:** CMake with `-DCMAKE_PREFIX_PATH="C:/Users/lnami/AppData/Local/Temp/klyrdb_phase13b_test"`
- **Result:** Configured and compiled in Release mode without warnings.
- **Execution:** Successfully performed database creation, table DDL, row insertions, `SELECT` queries, intentional error handling, rollback, commit via `TransactionGuard`, closure, and cross-session persistence verification across 4 rows.
- **Verdict:** **PASS**

### 5.2 Supplied Consumer 2: `transactions_and_constraints`
- **Source:** Extracted `examples/transactions_and_constraints`
- **Configuration:** CMake with `-DCMAKE_PREFIX_PATH="C:/Users/lnami/AppData/Local/Temp/klyrdb_phase13b_test"`
- **Result:** Configured and compiled cleanly.
- **Execution:** Successfully enforced duplicate `PRIMARY KEY` rejection, `NOT NULL` constraint rejection, `UNIQUE` constraint rejection, automatic rollback on violation via `TransactionGuard`, and post-rollback storage integrity verification.
- **Verdict:** **PASS**

### 5.3 Independent Consumer App (`phase13b_indep_app`)
A third, independent consumer application was authored in a separate directory (`C:\Users\lnami\AppData\Local\Temp\phase13b_independent_consumer`):
```cmake
cmake_minimum_required(VERSION 3.20)
project(phase13b_indep_app LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(KlyrDB REQUIRED)
add_executable(phase13b_indep_app main.cpp)
target_link_libraries(phase13b_indep_app PRIVATE KlyrDB::KlyrDB)
```

The application executed:
1. `klyrdb::Database::open("indep_test.kdb")`
2. `CREATE TABLE inventory (sku INTEGER PRIMARY KEY, item TEXT, qty INTEGER);`
3. Committed insertions (`Router`, `Switch`).
4. Rolled back insertion (`Hub`).
5. Reopened database from disk and verified that exactly 2 rows persisted and the rolled-back row was absent.

#### Path Leakage Audit:
The MSVC compiler/linker telemetry logs (`.command.1.tlog`) were inspected. Zero references to `D:\KlyrDB` were found. Linkage resolved strictly against the extracted `lib/klyrdb_storage.lib` and headers in `include/`.

---

## 6. Remaining Engine Limitations

The engine's functional limitations are documented transparently:
1. **Single-Writer Concurrency:** Serialized database write operations via coarse-grained table/database locking.
2. **Tuple Size Limit:** Maximum single-tuple payload of 4,072 bytes (no multi-page overflow / TOAST).
3. **SQL Surface:** No `JOIN` operators, subqueries, `GROUP BY`, `HAVING`, window functions, or aggregate functions in SQL expressions.
4. **Embedded Deployment:** Library and CLI only (no background network daemon).
5. **Operating System Packages:** Binary distribution archive is compiled for Windows x64. Linux users build from source via CMake.

---

## 7. Final Verdict

| Check | Requirement | Result |
|---|---|---|
| **Build & Test** | 51 / 51 CTest suites pass in Release | **PASS** |
| **Packaging Staging** | Exclude all intermediate build files | **PASS (0 build files)** |
| **Archive Integrity** | Match SHA-256 checksum file | **PASS (`58CF6568...`)** |
| **External Consumer 1** | Build & run `basic_cpp_app` | **PASS** |
| **External Consumer 2** | Build & run `transactions_and_constraints` | **PASS** |
| **Independent Consumer** | Direct `find_package(KlyrDB REQUIRED)` | **PASS** |
| **Path Isolation** | Zero source-tree path leakage | **PASS** |

**Final Release Status:** **PASS** (Ready for distribution candidate tagging upon user authorization).
