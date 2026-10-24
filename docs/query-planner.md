# KlyrDB — Query Planning & Optimization Engine

## 1. Architectural Overview

Phase 7 introduces an independent, cost-based query planning subsystem that decouples SQL query analysis from runtime execution.

The query execution flow is now structured as follows:

```
                      SQL Query String
                             │
                             ▼
                        [SqlLexer]
                             │
                             ▼
                        [SqlParser]
                             │
                             ▼
                      SelectStatement (AST)
                             │
                             ▼
                      [QueryPlanner]
                             │
                             ├── Table Catalog & Schema Resolution
                             ├── Page & Row Cardinality Estimation
                             ├── Candidate Plan Generation:
                             │     ├── TableScan (Full sequential scan)
                             │     ├── IndexSeek (Equality point lookup)
                             │     └── IndexRangeScan (Bounded range & ordered scan)
                             └── Cost-Based Selection (Min estimated work)
                             │
                             ▼
                         QueryPlan
                             │
                             ▼
                       [SqlExecutor]
                             ├── Access Path Execution (BPlusTree / SlottedPage)
                             ├── Residual Filter Evaluation
                             ├── In-Memory Sort (only if not satisfied by index)
                             ├── Limit Application (pushdown early-exit vs post-sort)
                             └── Column Projection -> QueryResult
```

---

## 2. Access Path Evaluation

The `QueryPlanner` models three distinct physical access paths:

### 2.1 Table Scan (`AccessPathType::TableScan`)
- Scans all slotted pages belonging to the target table via the buffer pool.
- Filters each tuple against the full set of binary predicates in the `WHERE` clause.
- Requires explicit in-memory sort if `ORDER BY` is present.
- Cannot push down `LIMIT` when an explicit sort is required.

### 2.2 Index Seek (`AccessPathType::IndexSeek`)
- Selected when an equality predicate (`col = value`) targets a column with an attached B+ tree index (e.g. Primary Key or Secondary Index).
- B+ tree lookup locates the exact matching `RecordId`s in $O(\log N)$ page operations.
- Residual predicates (predicates on other columns) are retained as post-seek filters.
- Trivially satisfies `ORDER BY` since a single-key lookup produces identical keys.

### 2.3 Index Range Scan (`AccessPathType::IndexRangeScan`)
- Selected when range comparison predicates (`<`, `<=`, `>`, `>=`) target an indexed column, or when an index is used to satisfy query ordering.
- Multiple range predicates on the same indexed column are consolidated into unified low/high bounds:
  - Low bound: greatest lower bound (`>`, `>=`) with strict/inclusive flag.
  - High bound: least upper bound (`<`, `<=`) with strict/inclusive flag.
- Traverses leaf pages sequentially using B+ tree leaf sibling links (`next_leaf`).

---

## 3. Cost Model & Cardinality Estimation

The planner uses a calibrated cost model based on page I/O, random record dereferencing, and CPU tuple processing:

$$C_{\text{TableScan}} = C_{\text{startup}} + (N_{\text{pages}} \times C_{\text{page\_io}}) + (N_{\text{rows}} \times C_{\text{cpu\_tuple}}) + C_{\text{sort}}$$

$$C_{\text{IndexSeek}} = (0.5 \times C_{\text{page\_io}}) + (N_{\text{est\_rows}} \times C_{\text{random\_io}}) + (N_{\text{est\_rows}} \times N_{\text{residuals}} \times C_{\text{cpu\_tuple}})$$

$$C_{\text{IndexRangeScan}} = (0.8 \times C_{\text{page\_io}}) + (N_{\text{est\_rows}} \times C_{\text{random\_io}}) + (N_{\text{est\_rows}} \times N_{\text{residuals}} \times C_{\text{cpu\_tuple}}) + C_{\text{sort}}$$

### Calibrated Cost Constants:
| Constant | Value | Description |
|---|---|---|
| `COST_SCAN_STARTUP` | `0.5` | Fixed overhead to initiate a sequential page scan |
| `COST_PAGE_IO` | `1.0` | Cost of fetching a page through the buffer pool |
| `COST_RANDOM_IO` | `0.1` | Cost of dereferencing a record via its `RecordId` |
| `COST_CPU_TUPLE` | `0.01` | CPU cost of inspecting and evaluating a row predicate |
| `COST_CPU_SORT` | `0.05` | CPU cost factor for in-memory sorting ($N \log_2 N$) |

### Unselective Index Scan Inversion
When a range predicate covers a large fraction of the table (e.g., $> 50\%$), the aggregate random I/O and tuple fetches exceed the cost of sequentially reading the compact table pages. The cost model accurately detects this threshold and prefers a full Table Scan over an unselective Index Range Scan.

---

## 4. Ordering & Limit Pushdown

### 4.1 Index-Assisted Ordering
If the query contains an `ORDER BY` clause and an index candidate covers the order column:
- ASC order is satisfied by standard forward leaf scan.
- DESC order is satisfied by reverse leaf scan (`scan_descending = true`).
- In both cases, expensive in-memory sort ($O(N \log N)$) is completely bypassed (`order_by_satisfied_by_index = true`).

### 4.2 Early-Exit Limit Pushdown
If `LIMIT k` is requested:
- When ordering is satisfied by an index (or no `ORDER BY` exists), `limit_pushed_down = true`.
- The access path terminates scanning immediately once $k$ matching tuples have been emitted.
- Subordinate pages are never fetched from disk or buffer pool.
