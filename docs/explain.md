# KlyrDB — EXPLAIN & EXPLAIN QUERY PLAN

## 1. Syntax

KlyrDB supports the standard `EXPLAIN` and `EXPLAIN QUERY PLAN` commands:

```sql
EXPLAIN SELECT ... ;
EXPLAIN QUERY PLAN SELECT ... ;
```

Both forms inspect the cost-based query planner and format the chosen physical execution plan without reading table records or executing mutating statements.

---

## 2. Safety & Read-Only Guarantees

`EXPLAIN` is strictly restricted to read-only queries (`SELECT`).
Attempting to explain mutating or DDL statements will be rejected immediately with an appropriate error status:

```sql
klyrdb> EXPLAIN INSERT INTO users VALUES (1, 'Alice');
Error: Unsupported SQL statement: EXPLAIN is only supported for read-only queries (SELECT)

klyrdb> EXPLAIN UPDATE users SET age = 30;
Error: Unsupported SQL statement: EXPLAIN is only supported for read-only queries (SELECT)

klyrdb> EXPLAIN DELETE FROM users WHERE id = 1;
Error: Unsupported SQL statement: EXPLAIN is only supported for read-only queries (SELECT)
```

Running `EXPLAIN` does not modify database state, allocate pages, or generate write-ahead log records.

---

## 3. Output Format

The output of `EXPLAIN` is returned as a single-column tabular result set with column header `QUERY PLAN`:

### 3.1 Point Equality Lookup (Index Seek)
```sql
klyrdb> EXPLAIN SELECT * FROM users WHERE id = 42;
```
```text
Query Plan
----------
Index Seek
  Table: users
  Index: pk_users_id
  Predicate: id = 42
  Estimated Rows: 1
  Estimated Cost: 0.60
```

### 3.2 Range Scan with Residual Filter and Limit Pushdown
```sql
klyrdb> EXPLAIN SELECT * FROM users WHERE age >= 21 AND age < 65 AND name = 'Alice' ORDER BY age ASC LIMIT 10;
```
```text
Query Plan
----------
Index Range Scan
  Table: users
  Index: idx_users_age
  Predicate: age >= 21 AND age < 65
  Residual Filter: name = Alice
  Order: age ASC (Satisfied by index)
  Limit: 10 (Pushed down)
  Estimated Rows: 10
  Estimated Cost: 2.10
```

### 3.3 Full Table Scan with Explicit Sort
```sql
klyrdb> EXPLAIN SELECT * FROM users WHERE name = 'Alice' ORDER BY department DESC LIMIT 5;
```
```text
Query Plan
----------
Table Scan
  Table: users
  Residual Filter: name = Alice
  Order: department DESC (Explicit Sort)
  Limit: 5 (Post-sort)
  Estimated Rows: 100
  Estimated Cost: 15.20
```

---

## 4. Plan Interpretation Guide

| Field | Meaning |
|---|---|
| **Access Path** (`Table Scan`, `Index Seek`, `Index Range Scan`) | The root physical method used to access tuples. |
| **Table** | The target database table being queried. |
| **Index** | Name of the index utilized for seek or range scan. |
| **Predicate** | Exact condition or range bounds evaluated by the index. |
| **Residual Filter** | Predicates not covered by the index, applied after tuple fetch. |
| **Order** | Column and direction (`ASC`/`DESC`), with note if satisfied by index or requiring explicit in-memory sorting. |
| **Limit** | `Pushed down` indicates scan terminates after $k$ rows; `Post-sort` indicates all candidates must be sorted first. |
| **Estimated Rows** | Planner's expected tuple cardinality. |
| **Estimated Cost** | Estimated work units in I/O and CPU time. |
