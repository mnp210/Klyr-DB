# KlyrDB — SQL Language, Parser & Query Execution Engine

## 1. Architecture Overview

KlyrDB Phase 4 implements an educational relational SQL execution pipeline directly atop KlyrDB's native C++20 storage foundation (slotted pages, buffer pool, Free-Space Map, and B+ Tree indexes).

```text
  +------------------+
  |    SQL Query     |  "SELECT name, age FROM users WHERE age >= 25;"
  +--------+---------+
           |
           v
  +------------------+
  |     SqlLexer     |  Tokenizes SQL string into keyword/literal/operator stream
  +--------+---------+
           |
           v
  +------------------+
  |    SqlParser     |  Recursive-descent parser producing typed AST Statements
  +--------+---------+
           |
           v
  +------------------+
  |   SqlExecutor    |  Coordinates TableCatalog, RecordManager, and BPlusTree
  +--------+---------+
           |
    +------+------+
    |             |
    v             v
[Index Lookup] [Table Scan]
    |             |
    +------+------+
           |
           v
  +------------------+
  | Filter & Project |  Evaluates WHERE, ORDER BY, LIMIT, and column projection
  +--------+---------+
           |
           v
  +------------------+
  |   QueryResult    |  ASCII Table Formatter / Row Counter
  +------------------+
```

---

## 2. SQL Language Grammar & Scope

### Data Definition Language (DDL)
- `CREATE TABLE <table_name> ( <col_def>, ... [, <table_constraint>, ...] );`
  - Column Types: `INTEGER` (64-bit signed int), `TEXT` (variable-length UTF-8 string).
  - Column Constraints:
    - `NOT NULL`: Rejects NULL or omitted values.
    - `PRIMARY KEY`: Single-column primary key backed by a unique B+ tree index.
    - `UNIQUE`: Single-column unique constraint backed by a unique B+ tree index.
    - `[FOREIGN KEY] REFERENCES <parent_table>(<parent_column>)`: Foreign key reference.
  - Table Constraints:
    - `PRIMARY KEY (<column>)`
    - `UNIQUE (<column>)`
    - `FOREIGN KEY (<column>) REFERENCES <parent_table>(<parent_column>)`
- `CREATE [UNIQUE] INDEX <index_name> ON <table_name> ( <column_name> );`
- `DROP TABLE <table_name>;` (Protected if referenced by foreign keys in other tables)
- `DROP INDEX <index_name> [ON <table_name>];` (Protected if enforcing PRIMARY KEY or UNIQUE)

### Data Manipulation Language (DML)
- `INSERT INTO <table_name> [( <col1>, <col2>... )] VALUES ( <val1>, <val2>... );`
  - Supports explicit column lists (with reordering and omitted nullable columns defaulting to NULL).
  - Supports `NULL` literal values.
  - Validates `NOT NULL`, `PRIMARY KEY`, `UNIQUE`, and `FOREIGN KEY` referential integrity before committing rows.
- `SELECT * | <col1>, <col2>... FROM <table_name> [WHERE <predicate> AND ...] [ORDER BY <col> [ASC|DESC]] [LIMIT <n>];`
  - Predicates supported: `=`, `!=`, `<>`, `<`, `<=`, `>`, `>=` connected with `AND`.
  - Distinguishes `NULL` from `""` and `0`.
- `UPDATE <table_name> SET <col> = <val>, ... WHERE <predicate> AND ...;`
  - Pre-validates all candidate updated rows against constraints before applying mutations.
  - Enforces `RESTRICT` / `NO ACTION` if updating a parent key referenced by child rows.
- `DELETE FROM <table_name> WHERE <predicate> AND ...;`
  - Enforces `RESTRICT` / `NO ACTION` if deleting a parent row referenced by child rows.
- `UPDATE <table_name> SET <col1> = <val1>, ... WHERE <predicate>;`
  - **Safety constraint**: `UPDATE` without `WHERE` is strictly rejected.
- `DELETE FROM <table_name> WHERE <predicate>;`
  - **Safety constraint**: `DELETE` without `WHERE` is strictly rejected.

### Query Plan Inspection
- `EXPLAIN [QUERY PLAN] SELECT ... ;`
  - Formats and displays the query planner's chosen physical access path, estimated cost, and estimated rows without executing data-modifying mutations.

---

## 3. Cost-Based Query Planning (`QueryPlanner`)

In Phase 7, query planning is decoupled from execution via `QueryPlanner`:
1. **Candidate Plan Generation**:
   - `TableScan`: Full sequential scan across slotted pages.
   - `IndexSeek`: Logarithmic B+ tree point seek on primary key or secondary index matching equality predicates (`=`).
   - `IndexRangeScan`: B+ tree leaf range traversal for bounded inequalities (`<`, `<=`, `>`, `>=`) or index-ordered scans.
2. **Multi-Predicate Conjunction & Residual Filters**:
   - Merges multiple range bounds on the same indexed column into unified lower and upper limits.
   - Retains unindexed filter conditions as residual predicates evaluated on fetched records.
3. **Calibrated Cost Model**:
   - Compares estimated I/O, record fetching, and CPU costs across all candidates.
   - Detects unselective index range scans (> 50% of table) and falls back to sequential Table Scan.
4. **ORDER BY Elimination & LIMIT Pushdown**:
   - Leverages forward (ASC) and reverse (DESC) leaf traversal to eliminate in-memory sorting.
   - Pushes down `LIMIT` into access paths to terminate execution early upon reaching target row count.

---

## 4. Query Execution Engine (`SqlExecutor`)

### Index Selection Strategy
When executing `SELECT`, `UPDATE`, or `DELETE`:
1. The executor inspects the `WHERE` clause for an equality comparison (`=`) against an indexed column.
2. **Index Path**: If an equality predicate matches an attached index, the executor queries the `BPlusTree::find(key)` in $O(\log N)$ time, directly retrieving the candidate `RecordId`s. It fetches only those candidate pages from the buffer pool and checks remaining filter conditions.
3. **Table Scan Fallback**: If no index is applicable, the executor performs a linear scan across slotted pages `1 .. N-1`, inspecting each active slot for matching `table_id` and evaluating the `WHERE` expression.

### Index Maintenance During Writes
- **INSERT**: Inserts new row into storage via `RecordManager`, extracts values for all attached indexes, and inserts `(IndexKey, RecordId)` into each index B+ tree.
- **UPDATE**: If an updated column has an attached index, the old `(IndexKey, RecordId)` is removed from the B+ tree, the row is updated in slotted storage, and the new `(IndexKey, RecordId)` is inserted.
- **DELETE**: Removes entries for the deleted record from all attached indexes and marks the slotted page slot as a tombstone.
- **Root Synchronization**: In all tree operations, if a node split or merge causes the B+ tree root page ID to change, the root change callback automatically updates the `TableCatalog` and `IndexCatalog`.

### In-Memory Sorting & Pagination
- **ORDER BY**: After candidate rows are filtered, they are sorted using `std::stable_sort` on the specified column's `SqlValue` (comparing integers numerically and strings lexicographically).
- **LIMIT**: The row buffer is truncated to `min(rows.size(), limit)`.

---

## 4. Interactive SQL Shell (`SqlShell`)

The interactive shell provides a SQLite-style REPL:
- Dot commands:
  - `.help`: Displays help menu with syntax reference.
  - `.tables`: Lists all tables in the database.
  - `.schema [table]`: Shows table schema and attached indexes.
  - `.indexes [table]`: Shows index roots and indexed columns.
  - `.quit` or `.exit`: Closes the shell session.
- Output Formatting: Formats tabular query results as cleanly aligned ASCII tables:
  ```text
  +----+---------+-----+
  | id | name    | age |
  +----+---------+-----+
  | 1  | Alice   | 24  |
  | 2  | Bob     | 29  |
  | 3  | Charlie | 35  |
  +----+---------+-----+
  3 rows in set.
  ```
