# KlyrDB Logical SQL & CSV Migration Engine

## 1. Logical SQL Dumps (`LogicalExporter` & `LogicalImporter`)

The logical export/import engine produces human-readable, portable SQL scripts that can recreate schemas, constraints, indexes, and records across different database environments.

### 1.1 Topological Table Ordering (Dependency DAG)
Relational databases with Foreign Key constraints cannot create or insert tables in arbitrary order. `LogicalExporter` inspects foreign key relationships in `TableCatalog` and builds a directed dependency graph:
- A directed edge $A \to B$ indicates that Table $A$ references Table $B$ (i.e. $B$ must exist before $A$).
- The engine computes an in-degree topological sort (Kahn's Algorithm), guaranteeing parent tables (`departments`) appear before child tables (`employees`).

### 1.2 Typed Literal Formatting & NULL Fidelity
- **Integers:** Formatted as raw numbers (`42`, `-100`).
- **Strings:** Quoted with single quotes (`'Engineering'`). Internal quotes are escaped using standard SQL doubling (`'O''Reilly'`).
- **NULLs:** Formatted as explicit keyword `NULL` (`INSERT INTO tbl VALUES (1, NULL);`), maintaining distinct semantics from empty strings (`''`).

### 1.3 Transactional Chunking & DDL Execution
- `CREATE TABLE` DDL statements are executed outside explicit transaction blocks (matching KlyrDB catalog DDL requirements).
- Data records are grouped inside `BEGIN TRANSACTION; ... COMMIT;` blocks to ensure high-throughput batch insertions and crash safety.
- Standalone secondary indexes (`CREATE INDEX`) are exported after table data so bulk loads do not incur index balancing overhead on every row.

### 1.4 Streaming Parser & Transactional Rollback
`LogicalImporter` utilizes `StatementStreamReader` to parse statements incrementally without loading multi-gigabyte dump files into memory:
- If `stop_on_error` is enabled (default), any failed SQL statement triggers an immediate rollback of the current transaction and cleans up the staging database, leaving the target path untouched.

---

## 2. RFC 4180 CSV Migration (`CsvMigrator`)

The CSV migration subsystem allows exporting and importing individual database tables using standard Comma-Separated Values format.

### 2.1 RFC 4180 Compliance & Null Sentinel
- **Header Row:** Field names are emitted as the first line.
- **Quoting:** Fields containing commas (`,`), quotes (`"`), or newlines (`\n` / `\r\n`) are enclosed in double quotes. Internal double quotes are escaped by doubling (`""`).
- **Explicit NULL vs Empty String:**
  - `\N`: Unquoted backslash-N represents a database `NULL`.
  - `""` or empty token: Represents an empty string (`TEXT`).
- **Multiline Streaming Parser:** The parser tracks quote parity across line boundaries, seamlessly supporting multi-line text records.
- **Relational Constraint Enforcement:** Records imported via CSV pass through KlyrDB's table constraint engine, enforcing NOT NULL, UNIQUE, CHECK, PRIMARY KEY, and FOREIGN KEY validations.

---

## 3. CLI Commands

### SQL Dump & Import
```bash
# Dump full database to SQL
klyrdb dump /var/lib/klyrdb/sales.kdb /var/backups/sales.sql

# Import SQL dump into a new database
klyrdb import /var/backups/sales.sql /var/lib/klyrdb/sales_imported.kdb
```

### CSV Export & Import
```bash
# Export single table to CSV
klyrdb export-csv /var/lib/klyrdb/sales.kdb customers /var/exports/customers.csv

# Import CSV into existing table
klyrdb import-csv /var/exports/customers.csv /var/lib/klyrdb/sales.kdb customers
```
