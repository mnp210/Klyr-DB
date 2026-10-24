# KlyrDB — Known Architecture & Engine Limitations

**Version:** 0.12.0  
**Status:** Release Candidate  
**Engineering Review Date:** 2026-10-24  

---

## 1. Concurrency Model & Locking

- **Single-Writer / Multiple-Reader (SWMR):** KlyrDB supports concurrent readers through shared locking, but only a single active writer at any time across the database.
- **Process-Level Exclusivity:** A companion `.lock` file uses OS-level advisory locks (`LockFileEx` on Windows, `fcntl` on POSIX). Running multiple write-heavy processes simultaneously will serialize or encounter lock acquisition timeouts if configured with timeouts.
- **Granularity:** Page-level latches are used for buffer pool frame management, but transactional updates operate under a coarse-grained table/database exclusive lock to guarantee crash-safe WAL ordering.

---

## 2. SQL Grammar & Execution Capabilities

- **Supported DDL:**
  - `CREATE TABLE [name] (col TYPE [PRIMARY KEY | NOT NULL | UNIQUE | CHECK(...) | REFERENCES table(col)], ...);`
  - `DROP TABLE [name];`
  - `CREATE [UNIQUE] INDEX [name] ON [table]([col]);`
  - `DROP INDEX [name];`
- **Supported DML:**
  - `INSERT INTO [table] VALUES (...);`
  - `SELECT [col1, col2 | *] FROM [table] [WHERE ...] [ORDER BY col [ASC|DESC]] [LIMIT n];`
  - `UPDATE [table] SET col = val [WHERE ...];` (WHERE clause is required by default for safety)
  - `DELETE FROM [table] [WHERE ...];`
- **Supported Transactions:**
  - `BEGIN;`
  - `COMMIT;`
  - `ROLLBACK;`
- **Unsupported SQL Features:**
  - No `JOIN` operators (relational queries must currently query individual tables or use application-side joining).
  - No aggregate functions (`COUNT`, `SUM`, `AVG`, `MIN`, `MAX`) inside SQL expressions; query rows directly and aggregate in application code or use `QueryResult::row_count()`.
  - No subqueries (`WHERE col IN (SELECT ...)`).
  - No `GROUP BY` or `HAVING` clauses.
  - No window functions (`OVER (...)`).
  - No `ALTER TABLE` schema mutations.

---

## 3. Data Types & Storage Limits

- **Supported Types:**
  - `INTEGER`: Signed 64-bit integer (`int64_t`).
  - `TEXT`: UTF-8 / ASCII variable-length string (`std::string`).
  - `NULL`: SQL null value representation across all types.
- **Page Size:** Fixed 4096-byte pages.
- **Maximum Record Size:** Single record payload cannot exceed slotted page free payload capacity (4072 bytes). Records spanning multiple pages (overflow pages / TOAST) are not supported.
- **Maximum Database Size:** 32-bit logical page numbering (`uint32_t`), providing a theoretical maximum single-file database size of \(2^{32} \times 4096 \approx 16\,\text{TB}\).
- **Index Key Constraints:** Secondary B+ tree keys support single-column `int64_t` or `string` keys up to 256 bytes. Composite / multi-column index keys are not supported.

---

## 4. Operational & Deployment Architecture

- **Embedded Engine Only:** KlyrDB is an embedded database library linked directly into host applications or used via the `klyrdb` CLI. There is no background daemon or TCP/IP network server protocol.
- **Zero Third-Party Dependencies:** Relies strictly on ISO C++20 standard libraries and operating system system calls.
