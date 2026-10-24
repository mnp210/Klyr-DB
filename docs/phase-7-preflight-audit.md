# KlyrDB Phase 7 Preflight Audit

Date: 2026-10-22
Baseline: 19/19 Test Suites Passing (100% pass rate, 2.50s execution time)

## 1. Current Query Execution Architecture
- Queries are executed in `SqlExecutor::execute_select` by directly invoking `find_matching_rows`.
- `find_matching_rows` currently contains hardcoded access path logic:
  - Scans `where.predicates` for the first predicate with `BinaryOp::Equal` that has an associated index.
  - If found, calls `BPlusTree::find(key)` to retrieve record IDs and fetches records individually.
  - If no equality predicate matches an index, it falls back to a full table scan across all slotted pages.
  - All other predicates (`<`, `<=`, `>`, `>=`, `!=`) always trigger a full table scan, completely ignoring existing B+ tree indexes.
- No query planning or optimization layer exists; query execution decisions are hardcoded inside `SqlExecutor`.
- `ORDER BY` is executed by loading all candidate rows into a `std::vector<RowWithId>` in memory and running `std::stable_sort`, even when an index exists on the ordering column.
- `LIMIT` is executed by truncating the in-memory result vector after sorting and filtering all matching rows.
- No `EXPLAIN` or query plan inspection capability exists.

## 2. B+ Tree Capabilities Audit
- `BPlusTree` in `include/klyrdb/index/bplus_tree.hpp` provides:
  - `find_one(key)`: Single point lookup.
  - `find(key)`: Multi-result lookup for duplicate keys.
  - `scan_range(low_key, high_key, inclusive_low, inclusive_high)`: Range scan returning `std::vector<IndexEntry>`.
  - `lower_bound(key)`: Returns iterator to first entry `>= key`.
  - `begin()` and `end()`: Forward leaf iterator.
- The index engine already supports range scans and ordered traversals natively.

## 3. SQL Grammar & AST Capabilities
- The SQL AST supports:
  - `SelectStatement`: `table_name`, `select_all`, `column_names`, `where` (conjunction of `BinaryPredicate`), `order_by` (`column_name`, `ascending`), `limit` (`has_limit`, `limit`).
  - Binary operators: `=`, `!=`, `<>`, `<`, `<=`, `>`, `>=`.
  - Value types: `Integer` (int64_t), `Text` (std::string), `Null`.
- Missing in AST/Parser:
  - `ExplainStatement` (`EXPLAIN [QUERY PLAN] <SELECT statement>`).

## 4. Phase 7 Objectives
1. Create a dedicated query planning and execution plan layer (`query_plan.hpp`, `query_planner.hpp`, `query_planner.cpp`).
2. Implement intelligent index selection for equality and range predicates (`<`, `<=`, `>`, `>=`).
3. Handle multi-predicate conjunctions: choose optimal indexed access path and apply remaining predicates as residual filters.
4. Implement lightweight cost estimation (distinguishing table scan, index seek, and index range scan costs).
5. Optimize `ORDER BY` and `LIMIT`: avoid sorting when index order satisfies requested order, push down `LIMIT` when safe.
6. Implement `EXPLAIN` query syntax and formatted execution plan output.
7. Preserve strict SQL and NULL semantics, transaction consistency, and backwards compatibility.
8. Add automated test suites and reproducible benchmarks.
