# KlyrDB Schema Constraints & Relational Integrity

## 1. Overview

Phase 6 introduces enterprise-grade schema constraints and relational integrity enforcement to KlyrDB. The constraint engine operates at the core storage and query execution layers, ensuring that data invariants are rigorously validated and upheld across all DDL and DML operations.

The constraint subsystem supports:
1. **NOT NULL**: Disallows `NULL` values or omitted column assignments for mandatory fields.
2. **PRIMARY KEY**: Single-column primary keys (column-level or table-level), automatically backed by a persistent unique B+ tree index. Rejects duplicates and `NULL` values.
3. **UNIQUE**: Single-column unique constraints (column-level or table-level), backed by a persistent unique B+ tree index. Rejects duplicate non-null values while permitting multiple `NULL` values per SQL standards.
4. **FOREIGN KEY**: Referential integrity between child and parent tables (column-level `REFERENCES` and table-level `FOREIGN KEY ... REFERENCES`). Validates parent key existence and enforces `RESTRICT` / `NO ACTION` semantics on parent deletions and updates.
5. **DDL & Index Protection**: Prevents dropping constraint-enforcing indexes via `DROP INDEX` and prevents dropping referenced parent tables via `DROP TABLE`.
6. **Multi-Row & Transaction Atomicity**: Pre-validates all candidate mutations before applying changes; rolls back cleanly on any constraint violation without leaving partial rows or dangling index entries.

---

## 2. NULL Semantics & Representation

In KlyrDB, `NULL` is treated as a distinct state of missing or inapplicable data:

* **Logical Representation (`SqlValue`)**:
  - `SqlType::Null = 0x00`.
  - Backed by `std::monostate` in the value variant.
  - Distinguishes `NULL` from an empty string `""` (which is a valid `SqlType::Text`) and zero `0` (which is a valid `SqlType::Integer`).
  - Optional underlying type hint preserves intended target type (e.g. `SqlValue::null(SqlType::Integer)`).

* **Storage Serialization (`SqlRow`)**:
  - `0x00`: Serialized with a 1-byte type tag and 0 payload bytes.
  - On deserialization, `0x00` parses cleanly into `SqlValue::null(expected_type)`.

* **Comparisons & Predicates**:
  - Per SQL three-valued logic, `NULL == NULL` evaluates to equality in administrative contexts, but standard comparisons (`<`, `>`, `<=`, `>=`) evaluate to false/unknown.
  - Unique indexes treat each `NULL` as distinct: multiple `NULL` values do not collide in a `UNIQUE` column.

---

## 3. Persistent Schema Serialization

Constraint metadata is persisted directly inside `TableCatalog` on catalog database pages, preserving 100% backward compatibility with Phase 1–5 files.

### Binary Layout of Catalog Table Entry:

```text
+-------------------------------------------------------------------------------+
| Table ID (4B) | Name Len (2B) | Name (Var) | Page ID (4B)                     |
+-------------------------------------------------------------------------------+
| Column Count (2B)                                                             |
|   For each column: Name Len (2B) | Name (Var) | Type Tag (1B)                 |
+-------------------------------------------------------------------------------+
| Index Count (2B)                                                              |
|   For each index:                                                             |
|     Idx Name Len (2B) | Idx Name (Var) | Col Name Len (2B) | Col Name (Var)   |
|     Col Idx (2B) | Root Page ID (4B) | Is Unique (1B)                         |
+-------------------------------------------------------------------------------+
| Constraint Count (2B)  [Phase 6 Extension]                                    |
|   For each constraint:                                                        |
|     Type (1B): 0=NotNull, 1=PrimaryKey, 2=Unique, 3=ForeignKey                |
|     Name Len (2B) | Name (Var)                                                |
|     Col Name Len (2B) | Col Name (Var)                                        |
|     Ref Table Len (2B) | Ref Table (Var)                                      |
|     Ref Col Len (2B) | Ref Col (Var)                                          |
|     Enforcement Index Len (2B) | Enforcement Index (Var)                      |
+-------------------------------------------------------------------------------+
```

### Backward Compatibility:
When opening legacy Phase 4/5 database files, `TableCatalog::load_from_page()` checks `if (entry_ptr + 2 <= entry_end)`. If the constraint count header is absent, the table initializes with an empty constraint vector, allowing legacy databases to load seamlessly without corruption or migration scripts.

---

## 4. Constraint Enforcement Mechanisms

### A. NOT NULL
- **INSERT**: Both positional and named inserts validate that every column marked `NOT NULL` receives a non-null value. Omitted columns default to `NULL` and trigger `StatusCode::ConstraintViolation` if defined as `NOT NULL`.
- **UPDATE**: SET clauses attempting to assign `NULL` to a `NOT NULL` column are rejected prior to mutating storage.

### B. PRIMARY KEY
- Enforces uniqueness and non-nullability.
- Automatically generates and attaches a unique B+ tree index named `pk_<table_name>_<col_name>`.
- Rejects table creation if multiple primary keys or composite primary keys are specified.
- Validates on `INSERT` and `UPDATE` that the key does not collide with existing records.

### C. UNIQUE
- Single-column uniqueness enforced by a backing unique B+ tree index named `uk_<table_name>_<col_name>`.
- Multiple `NULL` entries are permitted without violation per the SQL standard.
- Duplicate non-null values on `INSERT` or `UPDATE` return `StatusCode::ConstraintViolation`.

### D. FOREIGN KEY & Referential Integrity
- Validates at schema creation time that the referenced table and column exist, have matching data types, and the referenced column is either a `PRIMARY KEY` or `UNIQUE`.
- **Child Side (INSERT & UPDATE)**: When inserting or updating a child row with a non-null foreign key value, the executor validates that a matching row exists in the parent table.
- **Parent Side (RESTRICT / NO ACTION)**:
  - On `DELETE`: Rejects deletion of any parent row referenced by existing child rows.
  - On `UPDATE`: Rejects key modification of any parent row referenced by existing child rows.
  - Self-referencing tables allow deletion when parent and child are deleted in the same statement.

### E. DDL & Index Protection
- `DROP INDEX`: Cannot drop an index that enforces a `PRIMARY KEY` or `UNIQUE` constraint.
- `DROP TABLE`: Cannot drop a parent table if foreign keys from other tables reference it.

---

## 5. Transaction & Atomicity Guarantees

- **Pre-validation**: All operations (`INSERT`, `UPDATE`, `DELETE`) inspect and validate constraints across all affected rows *before* modifying records in the buffer pool, slotted pages, or B+ tree indexes.
- **Rollback Consistency**: If any row in a multi-row statement violates a constraint, the entire statement is aborted. In explicit transactions, issuing `ROLLBACK` completely restores the database to its pre-transaction state via WAL undo logging.
