# KlyrDB Query Profiling & EXPLAIN ANALYZE

## 1. Overview

While `EXPLAIN SELECT ...` produces a static compile-time query execution plan based on heuristic cost models, `EXPLAIN ANALYZE SELECT ...` physically executes the query, collects high-resolution runtime telemetry, and produces a combined report comparing estimated metrics against actual execution statistics.

---

## 2. Syntax & Grammar

The SQL parser supports `EXPLAIN` and `EXPLAIN ANALYZE` for `SELECT` queries:

```sql
EXPLAIN SELECT * FROM users WHERE age > 21;
EXPLAIN ANALYZE SELECT id, name FROM users WHERE id = 42;
EXPLAIN ANALYZE SELECT * FROM orders WHERE total >= 100 ORDER BY total DESC LIMIT 10;
```

Attempting to execute `EXPLAIN ANALYZE` on data-modifying statements (`INSERT`, `UPDATE`, `DELETE`, `CREATE`, `DROP`) returns an informative error to prevent unintentional mutations during profiling.

---

## 3. High-Resolution Timing & Telemetry

Profiling is instrumented using standard C++ `std::chrono::steady_clock` with microsecond resolution:
- **Planning Time (`planning_time_us`)**: Time taken by `QueryPlanner::create_plan()` to resolve catalogs, assess available B+ tree indexes, compare access paths, and synthesize physical plan nodes.
- **Execution Time (`execution_time_us`)**: Total duration of physical plan execution, including index traversal, record fetching, predicate filtering, projection, in-memory sorting, and row assembly.
- **Rows Examined (`rows_examined`)**: Total records scanned from slotted pages or index leaves during execution.
- **Actual Rows (`actual_rows`)**: Final cardinality of the result set delivered to the client.

---

## 4. Output Format & Structure

Executing `EXPLAIN ANALYZE` returns a single text column named `QUERY PLAN & EXECUTION SUMMARY`:

```
+-------------------------------------------------------------------------------+
| QUERY PLAN & EXECUTION SUMMARY                                                |
+-------------------------------------------------------------------------------+
| Query Plan                                                                    |
| ----------                                                                    |
| Index Seek                                                                    |
|   Table: users                                                                |
|   Index: pk_users_id                                                          |
|   Predicate: id = 1                                                           |
|   Estimated Rows: 1                                                           |
|   Estimated Cost: 0.60                                                        |
|                                                                               |
| Execution Summary                                                             |
| -----------------                                                             |
|   Actual Rows:   1                                                            |
|   Rows Examined: 1                                                            |
|   Planning Time: 242 us                                                       |
|   Exec Time:     220 us                                                       |
+-------------------------------------------------------------------------------+
```

### Table Scan vs Index Scan Profiling

For a table scan query:
```sql
EXPLAIN ANALYZE SELECT * FROM users WHERE age > 25;
```
Output:
```
Query Plan
----------
Table Scan
  Table: users
  Filter: age > 25
  Estimated Rows: 100
  Estimated Cost: 10.20

Execution Summary
-----------------
  Actual Rows:   14
  Rows Examined: 100
  Planning Time: 85 us
  Exec Time:     310 us
```
Notice that:
- `Estimated Rows` reflects catalog cardinality estimates.
- `Rows Examined` indicates that all 100 table records were scanned.
- `Actual Rows` indicates that 14 records matched the filter condition.

---

## 5. Table & Column Statistics (`ANALYZE`)

To ground query planning in real database statistics without requiring expensive table scans during planning, KlyrDB provides `TableStatistics`:
- `analyze_table(bpm, rm, tc, ic, table_id)`:
  - Scans table slotted pages.
  - Computes row count and page count.
  - Analyzes per-column null counts, distinct values, and min/max key bounds.
  - Caches results in `TableStatisticsRegistry`.
- `QueryPlanner::estimate_table_rows()` checks `TableStatisticsRegistry` first ($O(1)$ fast path). If no cached statistics are present, it falls back to inspecting the free-space map page count ($O(\text{pages})$ heuristic).
