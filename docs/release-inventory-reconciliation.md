# KlyrDB v0.12.0 — Release Inventory & Discrepancy Reconciliation

**Date of Audit:** 2026-10-24  
**Project:** KlyrDB C++20 Database Engine  
**Release Candidate Version:** `v0.12.0`  
**Archive File:** `dist/klyrdb-0.12.0-windows-x64.zip`  
**Checksum File:** `dist/klyrdb-0.12.0-windows-x64.zip.sha256`  
**Expected SHA-256:** `58CF6568E9FB5DF8B37A865651A5E99218EDE451F3A9523689878DD4A1242DEA`  
**Lead Evaluator:** Senior C++ Release Engineer  
**Final Status:** **PASS**

---

## 1. Executive Summary

An independent, byte-level inventory audit of the Windows x64 distribution package `dist/klyrdb-0.12.0-windows-x64.zip` was conducted to reconcile conflicting numbers reported across previous release reports.

### Primary Audit Findings:
1. **Archive Integrity:** The actual archive file size is **4,751,052 bytes** on disk, and its SHA-256 hash matches the expected hash (`58CF6568...`) and the companion `.sha256` file bit-for-bit.
2. **Total Entry Count:** The ZIP archive contains **131 total entries** (130 files + 1 directory entry `lib\cmake\`).
3. **Core Binary Sizes:**
   - `bin/klyrdb.exe`: **882,176 bytes** uncompressed (~861.5 KB), **395,490 bytes** compressed (44.8% ratio).
   - `lib/klyrdb_storage.lib`: **27,445,016 bytes** uncompressed (~26.17 MB), **4,134,952 bytes** compressed (15.1% ratio).
4. **Resolution of Prior Discrepancy:**
   - In `docs/phase-13b-release-verification.md` Section 4.3, the phrases "CLI binary (362 KB)" and "Static library (2.87 MB)" were inadvertently copied from the historical Phase 13A report (which evaluated an earlier Phase 12 compilation) rather than reflecting the actual byte sizes of the Release binaries.
   - In `docs/phase-13b-release-verification.md` Section 4.3, the documentation count was stated as 54 instead of 56 (due to two new Phase 13 tracking docs added before packaging), and examples were loosely stated as 10 (counting 8 files + 2 directory paths).
   - The detailed audit in `docs/release-artifact-audit-v0.12.0.md` accurately observed the true physical counts: 131 total entries, 56 docs, 8 example files, 59 headers, 4 CMake package files, 1 CLI executable, 1 static library, and 1 root README.

---

## 2. Archive File Properties & Independent SHA-256 Calculation

```powershell
$zipPath = "D:\KlyrDB\dist\klyrdb-0.12.0-windows-x64.zip"
Get-FileHash -Path $zipPath -Algorithm SHA256
Get-Content "$zipPath.sha256"
```

| Property | Measured Value | Expected / Specification | Status |
|---|---|---|---|
| **Archive File Size** | `4,751,052 bytes` (~4.53 MB) | `4,751,052 bytes` | **EXACT MATCH** |
| **Sum of Compressed Entries** | `4,733,232 bytes` | `4,733,232 bytes` | **EXACT MATCH** |
| **Zip Header / Central Dir Overhead** | `17,820 bytes` | `17,820 bytes` | **NORMAL ZIP METADATA** |
| **Total Uncompressed Entry Size** | `28,882,601 bytes` (~27.54 MB) | `28,882,601 bytes` | **EXACT MATCH** |
| **Overall Compression Ratio** | `16.39%` (83.61% space savings) | — | **EFFICIENT COMPRESSION** |
| **Computed SHA-256 Hash** | `58CF6568E9FB5DF8B37A865651A5E99218EDE451F3A9523689878DD4A1242DEA` | `58CF6568...` | **EXACT MATCH** |
| **Checksum File Hash** | `58CF6568E9FB5DF8B37A865651A5E99218EDE451F3A9523689878DD4A1242DEA` | `58CF6568...` | **EXACT MATCH** |
| **Last Modified (UTC)** | `2026-10-24T15:11:55.2434039Z` | — | **PRESERVED** |

---

## 3. Important Files Exact Size & Compression Ratio

Below is the verified inventory of core deliverables inside `dist/klyrdb-0.12.0-windows-x64.zip`:

| File Path in Archive | Uncompressed Size | Compressed Size | Compression Ratio | Purpose |
|---|---|---|---|---|
| `bin\klyrdb.exe` | **882,176 bytes** (861.50 KB) | **395,490 bytes** (386.22 KB) | 44.8% | Database CLI & interactive SQL shell |
| `lib\klyrdb_storage.lib` | **27,445,016 bytes** (26.17 MB) | **4,134,952 bytes** (3.94 MB) | 15.1% | Static C++20 engine library |
| `lib\cmake\KlyrDB\KlyrDBConfig.cmake` | 1,272 bytes | 578 bytes | 45.4% | CMake package configuration |
| `lib\cmake\KlyrDB\KlyrDBConfigVersion.cmake` | 2,830 bytes | 897 bytes | 31.7% | CMake version compatibility check |
| `lib\cmake\KlyrDB\KlyrDBTargets.cmake` | 4,493 bytes | 1,384 bytes | 30.8% | CMake imported targets definition |
| `lib\cmake\KlyrDB\KlyrDBTargets-release.cmake` | 1,338 bytes | 407 bytes | 30.4% | Target Release configuration map |
| `include\klyrdb\klyrdb.hpp` | 278 bytes | 149 bytes | 53.6% | Public master umbrella header |
| `include\klyrdb\database.hpp` | 2,008 bytes | 791 bytes | 39.4% | Public `klyrdb::Database` RAII handle |
| `include\klyrdb\connection.hpp` | 2,727 bytes | 801 bytes | 29.4% | Public `klyrdb::Connection` handle |
| `include\klyrdb\result.hpp` | 3,922 bytes | 1,065 bytes | 27.2% | Public `klyrdb::ResultSet` & monadic Result |
| `include\klyrdb\version.hpp` | 816 bytes | 337 bytes | 41.3% | Engine & storage format version API |
| `README.md` | 21,329 bytes | 8,487 bytes | 39.8% | Developer guide & architecture overview |
| `examples\basic_cpp_app\CMakeLists.txt` | 399 bytes | 265 bytes | 66.4% | Consumer 1 build script |
| `examples\basic_cpp_app\main.cpp` | 5,964 bytes | 1,759 bytes | 29.5% | Consumer 1 source code |
| `examples\transactions_and_constraints\CMakeLists.txt` | 344 bytes | 212 bytes | 61.6% | Consumer 2 build script |
| `examples\transactions_and_constraints\main.cpp` | 3,825 bytes | 1,258 bytes | 32.9% | Consumer 2 source code |

---

## 4. Disjoint Categorized Inventory & Counting Rules

### Counting Rules:
1. **File Entries vs Directory Entries:** Entries ending with `/` or `\` are directory markers. Entries not ending with `/` or `\` are file payloads.
2. **Mutually Exclusive Top-Level Bucketing:** Every entry is categorized strictly by its top-level path prefix.
   - `include/klyrdb/**`: Public C++20 header files.
   - `docs/**`: Technical documentation and test audit files.
   - `examples/**`: Consumer sample code and demo source files.
   - `lib/cmake/KlyrDB/**`: CMake exported package scripts.
   - `lib/**` (excluding `lib/cmake/`): Engine static library.
   - `bin/**`: CLI executable.
   - Root: Root `README.md`.

### Actual Verified Counts:

| Category | Type | Count | Exact Contents |
|---|---|---|---|
| **Root** | File | **1** | `README.md` |
| **`bin/`** | File | **1** | `bin\klyrdb.exe` |
| **`lib/` (Root)** | File | **1** | `lib\klyrdb_storage.lib` |
| **`lib/cmake/`** | Directory | **1** | Directory entry `lib\cmake\` |
| **`lib/cmake/KlyrDB/`** | Files | **4** | `KlyrDBConfig.cmake`, `KlyrDBConfigVersion.cmake`, `KlyrDBTargets.cmake`, `KlyrDBTargets-release.cmake` |
| **`include/`** | Files | **59** | 59 public headers under `include/klyrdb/` |
| **`docs/`** | Files | **56** | 56 `.md` files under `docs/` |
| **`examples/`** | Files | **8** | 4 standalone demo `.cpp` files (`constraints_demo.cpp`, `sql_demo.cpp`, `storage_demo.cpp`, `wal_demo.cpp`) + 2 in `basic_cpp_app/` + 2 in `transactions_and_constraints/` |
| **Total Files** | Files | **130** | Sum of all files across categories |
| **Total ZIP Entries** | All | **131** | **130 files + 1 directory entry (`lib\cmake\`)** |

---

## 5. Reconciliation Matrix of Prior Release Reports

| Metric / Item | Phase 13A Report (Preliminary Archive) | Phase 13B Report (Clean Candidate) | Latest Artifact Audit Report | Actual Verified Archive | Reconciliation / Root Cause Explanation |
|---|---|---|---|---|---|
| **Archive File Size** | 20,592,262 bytes | 4,751,052 bytes | 4,751,052 bytes | **4,751,052 bytes** | **Accurate.** Size dropped by ~76.9% after purging intermediate build artifacts. |
| **SHA-256 Hash** | `E2FD557A...` | `58CF6568...` | `58CF6568...` | `58CF6568...` | **Accurate.** Historical hash `E2FD557A...` properly superseded by clean candidate hash `58CF6568...`. |
| **Total Entry Count** | 314 entries | 131 entries | 131 entries | **131 entries** | **Accurate.** 183 intermediate files/dirs removed from `examples/**/build/`. |
| **`bin/klyrdb.exe` Size** | Stated as "362 KB" | Stated as "362 KB" | Stated as "882,176 bytes" | **882,176 bytes** uncompressed (395,490 bytes compressed) | **Discrepancy in 13B Narrative.** Phase 13B inadvertently retained the "362 KB" text from Phase 13A (an earlier build). The true uncompressed size is 882,176 bytes (~861.5 KB). |
| **`lib/klyrdb_storage.lib` Size**| Stated as "2.87 MB" | Stated as "2.87 MB" | Stated as "27,445,016 bytes" | **27,445,016 bytes** uncompressed (4,134,952 bytes compressed) | **Discrepancy in 13B Narrative.** Phase 13B inadvertently retained the "2.87 MB" text from Phase 13A (an earlier build). The true uncompressed size is 27,445,016 bytes (~26.17 MB). |
| **`docs/` File Count** | Stated as 54 | Stated as 54 | Stated as 56 | **56 files** | **Discrepancy in 13B Narrative.** Two new release tracking documents were added to `docs/` before Phase 13B packaging, increasing the count from 54 to 56. |
| **`examples/` File Count** | Stated as 193 (with build artifacts) | Stated as 10 | Stated as 8 | **8 files** | **Discrepancy in 13B Narrative.** Phase 13B counted 8 files + 2 subdirectory paths = 10 items. In the ZIP, only the 8 files exist. |

---

## 6. Extraction & Archive Health Verification

The archive was extracted to a clean temporary folder:
```powershell
Expand-Archive -Path D:\KlyrDB\dist\klyrdb-0.12.0-windows-x64.zip -DestinationPath C:\Users\lnami\AppData\Local\Temp\klyrdb_reconciliation_test -Force
```
- **Extracted File Count:** Exactly **130 files** extracted without decompression errors.
- **Extracted Directory Structure:** `bin/`, `docs/`, `examples/`, `include/`, `lib/`, `README.md`.
- **Integrity Validation:** Clean exit code `0`.

---

## 7. Final Verdict

### **Verdict: PASS**

1. The archive `dist/klyrdb-0.12.0-windows-x64.zip` is completely healthy, structurally intact, and uncorrupted.
2. The SHA-256 hash `58CF6568E9FB5DF8B37A865651A5E99218EDE451F3A9523689878DD4A1242DEA` matches the checksum file bit-for-bit.
3. All core deliverables (`klyrdb.exe`, `klyrdb_storage.lib`, 59 headers, 4 CMake package files, 56 documentation files, 8 example files) are present and verified.
4. The narrative discrepancies in the Phase 13B report have been fully identified, documented, and reconciled with exact physical byte measurements.
