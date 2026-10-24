# KlyrDB v0.12.0 Release Notes

**Release Tag:** `v0.12.0` (Release Candidate)  
**Release Date:** 2026-10-24  
**Target Commit:** `f769729` (Source & packaging baseline: `ab34daa73596ece6b22e8b395c58488d56feebb5`)  
**Distribution Archive:** `klyrdb-0.12.0-windows-x64.zip`  
**SHA-256 Checksum:** `58CF6568E9FB5DF8B37A865651A5E99218EDE451F3A9523689878DD4A1242DEA`  
**Status:** Pre-1.0 Experimental / Educational Embedded Relational Database Engine  

---

## 1. Project Overview & Intended Use

**KlyrDB** is an educational and experimental embedded relational database engine implemented from scratch in modern standard C++20 (`/std:c++20`, `/permissive-`, `/W4`). Inspired by SQLite's layered architectural model, KlyrDB provides a modular systems-programming implementation of page-based storage, slotted pages, buffer pool caching, B+ tree indexing, SQL parsing and execution, cost-based query optimization, Write-Ahead Logging (WAL), ARIES-style crash recovery, and physical backup and atomic restore.

> [!WARNING]
> **Pre-1.0 Experimental Engine:** KlyrDB is intended for systems study, research, and educational integration. It is **not** certified for mission-critical enterprise production deployments, and no claims of universal crash safety or commercial ACID certification are made.

---

## 2. Public C++20 SDK & CMake Integration

KlyrDB provides a clean, self-contained public developer interface under `<klyrdb/klyrdb.hpp>` with zero third-party dependencies:

- **`klyrdb::Database`**: RAII handle managing database storage, buffer pool frames, WAL logging, and catalogs. Supports `open(path, options)`, `checkpoint()`, `integrity_check()`, and direct `execute()`.
- **`klyrdb::Connection`**: Active connection session managing transaction boundaries (`begin_transaction()`, `commit()`, `rollback()`) and query dispatch.
- **`klyrdb::ResultSet`**: Tabular query results supporting case-insensitive column lookups (`row["name"]`), zero-based indexing (`row[0]`), and standard range-based iteration.
- **`klyrdb::TransactionGuard`**: RAII transaction guard that automatically aborts and rolls back uncommitted changes on scope exit unless `commit()` is explicitly invoked.
- **`klyrdb::version()`**: Programmatic inspection of semantic engine version (`0.12.0`), storage format version (`1`), and backup format version (`1`).

### CMake Integration (`find_package`)

Installed installations export standard CMake targets:

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyApplication CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(KlyrDB REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE KlyrDB::KlyrDB)
```

---

## 3. Verified Subsystems & Core Capabilities

All capabilities listed below are confirmed by automated regression suites and empirical benchmarks:

1. **Slotted-Page Storage Foundation:**
   - Fixed 4096-byte pages with big-endian binary encoding.
   - IEEE 802.3 CRC-32 checksums on every database page and WAL record.
   - Slotted page architecture with slot array compaction and slot reclamation.
   - Clock-Pro buffer pool manager with dirty page tracking and RAII `PageGuard`.
   - Free Space Map (FSM) tracking page allocation and capacity.
2. **Persistent B+ Tree Indexing:**
   - Single-column B+ tree indexing for `INTEGER` (`int64_t`) and `TEXT` (`std::string`) keys up to 256 bytes.
   - Dynamic node splitting, balance management, duplicate key support, and sequential leaf iteration.
3. **SQL Engine & Cost-Based Planner:**
   - DDL: `CREATE TABLE`, `DROP TABLE`, `CREATE [UNIQUE] INDEX`, `DROP INDEX`.
   - DML: `INSERT`, `SELECT` (projection, `WHERE`, `ORDER BY`, `LIMIT`), `UPDATE` (WHERE required), `DELETE`.
   - Cost-based query planner selecting between Table Scan, Index Seek, and Index Range Scan.
   - Index-assisted `ORDER BY` elimination and early-exit `LIMIT` pushdown.
   - `EXPLAIN` and `EXPLAIN ANALYZE` query execution profiling.
4. **Relational Constraints:**
   - `NOT NULL`, single-column `PRIMARY KEY`, single-column `UNIQUE`, and `CHECK` constraints.
   - `FOREIGN KEY` referential integrity with `RESTRICT` on `DELETE` and `UPDATE`.
   - ISO SQL compliant handling of `NULL` in unique constraints (multiple distinct `NULL` values permitted).
5. **Transactions & ARIES Crash Recovery:**
   - Single-writer / multiple-reader concurrency model with advisory OS-level file locking (`.lock`).
   - Write-Ahead Logging (`<dbname>.wal`) enforcing write-ahead invariants prior to dirty page flushes.
   - Multi-page reverse rollback on transaction abort.
   - 3-pass ARIES crash recovery (Analysis, Redo, Undo).
   - Recovery idempotence: durable `Abort` log records appended for undone transactions during crash recovery.
   - Safe checkpointing and safe WAL truncation retaining active transaction horizons.
6. **Disaster Recovery & Migration:**
   - Online physical hot backup producing standalone versioned snapshot directories (`manifest.json` + CRC-32).
   - Non-destructive `BackupVerifier` validating component checksums.
   - Transactional atomic restore executing inside staging directories with rollback safety.
   - RFC 4180 CSV export and import with typed `\N` null fidelity.
   - Topological DAG-ordered SQL export and replay.

---

## 4. Quick-Start Examples

### 4.1 CLI Quick-Start

The interactive shell and utility CLI is provided via `bin/klyrdb.exe`:

```powershell
# Open interactive SQL shell
./klyrdb.exe shell my_database.kdb

# Execute direct SQL statements
./klyrdb.exe exec my_database.kdb "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);"
./klyrdb.exe exec my_database.kdb "INSERT INTO users VALUES (1, 'Alice');"
./klyrdb.exe exec my_database.kdb "SELECT * FROM users;"

# Verify storage and index integrity
./klyrdb.exe check my_database.kdb

# Create physical backup
./klyrdb.exe backup my_database.kdb ./backup_20261024

# Verify backup archive
./klyrdb.exe verify-backup ./backup_20261024
```

### 4.2 C++ Public SDK Quick-Start

```cpp
#include <klyrdb/klyrdb.hpp>
#include <iostream>

int main() {
    // Open or create database
    auto db_res = klyrdb::Database::open("app.kdb");
    if (!db_res.is_ok()) {
        std::cerr << "Open error: " << db_res.status().message() << "\n";
        return 1;
    }
    auto db = std::move(db_res.value());

    // Create session connection
    auto conn_res = db->connect();
    auto conn = std::move(conn_res.value());

    // Schema DDL
    conn->execute("CREATE TABLE products (sku INTEGER PRIMARY KEY, name TEXT, price INTEGER);");

    // Transaction with RAII rollback guard
    {
        klyrdb::TransactionGuard txn(*conn);
        conn->execute("INSERT INTO products VALUES (101, 'Keyboard', 49);");
        conn->execute("INSERT INTO products VALUES (102, 'Mouse', 25);");
        txn.commit(); // Explicit commit
    }

    // Query result inspection
    auto res = conn->execute("SELECT * FROM products WHERE price < 50 ORDER BY sku ASC;");
    if (res.is_ok()) {
        for (const auto& row : res.value()) {
            std::cout << "Item: " << row["name"].as_text() 
                      << " ($" << row["price"].as_integer() << ")\n";
        }
    }

    db->close();
    return 0;
}
```

---

## 5. Verified Test Evidence & Platform Support

### 5.1 Test Execution Results (51 / 51 Passed)

All 51 CTest test suites pass cleanly across both Debug and Release configurations:

- **Debug Configuration (`D:\KlyrDB\build`):** 51 / 51 passed (16.30s real time, exit code 0).
- **Release Configuration (`D:\KlyrDB\build_release`):** 51 / 51 passed (15.90s real time, exit code 0).
- **Adversarial Hardening (`test_adversarial`):** Verified malformed WAL headers, corrupted magic signatures, malicious payload length allocations, torn writes, and recovery idempotence across 3 consecutive invocations.

### 5.2 Supported Platforms & Toolchains

| Platform / Toolchain | Status | Details |
|---|---|---|
| **Windows 11 x64 (MSVC 19.51 / BuildTools)** | **Tested & Verified** | Built and verified locally using CMake 4.3.1 and MSVC `/std:c++20`. |
| **Linux x64 (GCC 12 / Clang 15)** | **Intended / Portable** | Standard ISO C++20 code with POSIX `fcntl` / `fsync` implementations; targeted in `.github/workflows/ci.yml`. Binary distributions for Linux are not yet packaged locally. |

---

## 6. Known Architecture & Engine Limitations

1. **Single-Writer / Multi-Reader (SWMR):** Concurrent read operations scale via shared locking, but database mutations are serialized under an exclusive table/database lock.
2. **Fixed Page & Tuple Limits:** Page size is fixed at 4096 bytes. The maximum single-record payload size is 4072 bytes (records spanning multiple pages / overflow pages are not supported).
3. **Unsupported SQL Operators:**
   - No `JOIN` operators (relational queries must join in application logic).
   - No SQL aggregate functions inside query expressions (`COUNT(*)`, `SUM()`, `AVG()`).
   - No subqueries (`WHERE id IN (SELECT ...)`).
   - No `GROUP BY` or `HAVING` clauses.
   - No window functions (`OVER (...)`).
   - No schema alterations (`ALTER TABLE`).
4. **Index Key Constraints:** Secondary B+ tree keys support single-column `INTEGER` or `TEXT` keys up to 256 bytes. Composite/multi-column index keys are not supported.
5. **Embedded Deployment Only:** KlyrDB is designed as an embedded library linked into host processes or used via CLI. There is no client-server network daemon or wire protocol.

---

## 7. Storage Format & Compatibility

- **Storage Format Version:** `1` (Big-endian, 4096-byte pages, Page 0 Database Metadata).
- **Backup Format Version:** `1` (JSON manifest with SHA-256 and CRC-32 component records).
- **Forward Compatibility:** Upward migrations from Phase 5 databases (`demo_phase5.kdb`) open and verify with 0 integrity check errors.

---

## 8. Distribution Assets

- **Archive File:** `klyrdb-0.12.0-windows-x64.zip` (4,751,052 bytes)
- **Local Path:** `D:\KlyrDB\dist\klyrdb-0.12.0-windows-x64.zip`
- **SHA-256 Checksum:** `58CF6568E9FB5DF8B37A865651A5E99218EDE451F3A9523689878DD4A1242DEA`
- **Checksum File:** `D:\KlyrDB\dist\klyrdb-0.12.0-windows-x64.zip.sha256`
- **Packaging Note:** Verified clean distribution candidate (Phase 13B). Zero intermediate build files or MSBuild caches. Supersedes initial candidate archive (20,592,262 bytes, historical SHA-256 `E2FD557A...`).
