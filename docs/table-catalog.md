# KlyrDB — Table Catalog Architecture & Schema Persistence

## 1. Overview & Purpose

In KlyrDB Phase 4, the **Table Catalog** (`TableCatalog`) provides durable schema metadata management. It manages table schemas, column types, and associated B+ Tree index descriptors across database sessions without external dependencies.

Prior to Phase 4, the storage engine handled unstructured or single-table records. With the Table Catalog, KlyrDB supports multiple distinct relational tables (e.g., `users`, `products`) sharing the underlying buffer pool and slotted storage pages with full schema enforcement.

---

## 2. Table Catalog Physical Storage Format

The Table Catalog uses a dedicated database page type: `PageType::TableCatalog = 0x0009`.

### Root Tracking in Page 0 Metadata
To locate the catalog across database reopens without scanning the database file:
- Offset 36 of Page 0's database metadata payload stores `table_catalog_page_id` (`uint32_t` big-endian).
- If `table_catalog_page_id == INVALID_PAGE_ID` (0xFFFFFFFF), `TableCatalog::open_or_create` allocates and formats a new catalog page, updating Page 0.

### Catalog Page Binary Layout (4072-byte Payload)

```text
+--------------------------------------------------------------------------------+
| Catalog Header (12 bytes)                                                      |
|   - [0..1]   page_type               (uint16_t = 0x0009)                       |
|   - [2..3]   table_count             (uint16_t)                                |
|   - [4..7]   next_catalog_page_id    (uint32_t, INVALID_PAGE_ID if single)     |
|   - [8..11]  last_table_id           (uint32_t, monotonic counter for IDs)     |
+--------------------------------------------------------------------------------+
| Serialized Table Entries [12..N]                                               |
|   For each table:                                                              |
|     - entry_len            (uint16_t)                                          |
|     - table_id             (uint32_t)                                          |
|     - table_name_len       (uint16_t)                                          |
|     - table_name_bytes     (UTF-8, canonical lowercase)                        |
|     - column_count         (uint16_t)                                          |
|     - columns:                                                                 |
|         * col_name_len     (uint16_t)                                          |
|         * col_name_bytes   (UTF-8, canonical lowercase)                        |
|         * col_type         (uint8_t: 1 = INTEGER, 2 = TEXT)                   |
|     - index_count          (uint16_t)                                          |
|     - indexes:                                                                 |
|         * idx_name_len     (uint16_t) + bytes                                  |
|         * col_name_len     (uint16_t) + bytes                                  |
|         * col_index        (uint16_t)                                          |
|         * root_page_id     (uint32_t)                                          |
|         * is_unique        (uint8_t: 0 = non-unique, 1 = unique)               |
|     - constraint_count     (uint16_t, Phase 6 extension)                       |
|     - constraints:                                                             |
|         * type             (uint8_t: 0=NotNull, 1=PK, 2=Unique, 3=FK)          |
|         * name_len         (uint16_t) + bytes                                  |
|         * col_name_len     (uint16_t) + bytes                                  |
|         * ref_table_len    (uint16_t) + bytes                                  |
|         * ref_col_len      (uint16_t) + bytes                                  |
|         * enf_index_len    (uint16_t) + bytes                                  |
+--------------------------------------------------------------------------------+
```

### Backward Compatibility
Legacy tables lacking the `constraint_count` block load cleanly with empty constraints because `TableCatalog::load_from_page()` verifies `entry_ptr + 2 <= entry_end` before attempting to read constraints. Existing Phase 1–5 files remain 100% compatible.

---

## 3. Multi-Table Storage Disambiguation

Because all tables share the same storage pages via `RecordManager`, rows stored in slotted pages are prefixed with their owning `table_id`:

```text
[version: 1B] [table_id: 4B] [column_count: 2B] [column_tags & values...]
```

When performing a sequential table scan:
1. `SqlRow::peek_table_id(bytes)` checks bytes `[1..4]` in $O(1)$ time without decoding string or column values.
2. If `table_id != schema.table_id`, the row belongs to another table and is skipped immediately.
3. If `table_id == schema.table_id`, the row is decoded and validated against the schema.

---

## 4. Case-Sensitivity & Identifier Validation Policy

1. **Keywords**: Case-insensitive (`SELECT`, `select`, `SeLeCt`).
2. **Identifiers**: Table names, column names, and index names are case-insensitive. They are canonicalized to lowercase on ingestion and lookup.
3. **Identifier Syntax**:
   - Must begin with an ASCII letter (`a-z`, `A-Z`) or underscore (`_`).
   - May contain alphanumeric characters and underscores.
   - Cannot be empty.
4. **Validation Rules**:
   - Duplicate table names are rejected with `StatusCode::TableAlreadyExists`.
   - Duplicate column names within a `CREATE TABLE` definition are rejected with `StatusCode::DuplicateColumn`.
   - Tables must have at least one column.

---

## 5. Catalog Operations & Lifecycle

- `create_table(name, columns)`: Validates identifiers, verifies uniqueness, assigns monotonic `table_id`, persists to catalog page.
- `get_table(name)`: Case-insensitive lookup returning `TableSchema`.
- `get_table(table_id)`: Numeric ID lookup.
- `add_index_to_table(table_name, index_info)`: Records attached B+ Tree index metadata and root page ID in the table schema.
- `update_index_root(table_name, index_name, new_root_id)`: Synchronizes the index root page ID when tree splits or collapses occur.
- `drop_index_from_table(table_name, index_name)`: Removes index descriptor from the schema.
- `drop_table(table_name)`: Removes table schema and attached indexes from the catalog page.
