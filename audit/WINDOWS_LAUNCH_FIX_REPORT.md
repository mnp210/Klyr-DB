# KlyrDB — Windows Double-Click Launch Failure: Diagnosis, Fix & Audit Report

**Date:** 10 November 2026  
**Project:** KlyrDB Relational Database Engine (`D:\KlyrDB`)  
**Engine Version:** v0.12.0 (C++20)  
**Binary Target:** `build\Release\klyrdb.exe` & `build\Debug\klyrdb.exe`  
**GitHub Repository:** [mnp210/Klyr-DB](https://github.com/mnp210/Klyr-DB)  

---

## 1. Executive Summary

When attempting to launch `klyrdb.exe` by double-clicking the executable in Windows Explorer, the application appeared to flash momentarily and disappear without leaving an open, usable Command Prompt or SQL shell. In contrast, running the binary explicitly from an existing Command Prompt or PowerShell terminal (`klyrdb.exe sql <path>`) functioned normally.

This audit report documents the diagnosis, root cause, architectural fix, regression testing, and verification for the Windows double-click launch experience.

### Key Outcomes
- **Double-Click Experience Fixed:** Double-clicking `klyrdb.exe` directly in Windows Explorer now opens a persistent, visible console window running an interactive KlyrDB SQL shell.
- **Predictable Default Database:** When invoked with zero arguments (`argc == 1`), KlyrDB automatically locates its executable directory, creates a dedicated `data/` subdirectory (`<exe_dir>\data\klyrdb.kdb`), initializes the database if it does not exist, and starts the REPL with full persistence and automatic WAL recovery.
- **100% Backward Compatibility:** All existing subcommands (`sql`, `create`, `info`, `read`, `backup`, `restore`, `dump`, `import`, `integrity-check`, `--help`, `--version`, etc.) and automated test suites remain completely unchanged and backwards compatible.
- **Zero Scripting Regressions:** Standalone console detection (`GetConsoleProcessList`) ensures that batch scripts, CI pipelines, and piped commands never hang or block on exit.
- **Comprehensive Quality Assurance:** All **51 of 51 automated test suites pass (100%)** in both Release and Debug configurations.
- **Protected Release Assets:** The published Git tag `v0.12.0`, commit history, and release archives in `dist/` remain untouched.

---

## 2. Root Cause Analysis

### 2.1 Subsystem vs. Windows Explorer Process Lifecycle
`klyrdb.exe` is built with the Windows Console Subsystem (`IMAGE_SUBSYSTEM_WINDOWS_CUI`, Subsystem 3).

1. **Terminal Launch (CMD / PowerShell):**
   When launched from an existing terminal, the operating system executes `klyrdb.exe` inside the caller's console session. When the process finishes, the caller's console session remains alive and returns to the command prompt.
2. **Explorer Double-Click:**
   Windows Explorer does not have a console session. When a user double-clicks a `WINDOWS_CUI` binary, the Windows kernel and `conhost.exe` (or Windows Terminal) create an ephemeral console window solely for the lifetime of that process. As soon as the root process terminates, Windows instantly destroys the console window.

### 2.2 The Argument Handling Defect
In `src/main.cpp`:
```cpp
int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
```
When double-clicked from Windows Explorer, `argc == 1` (only `argv[0]`, the executable path, is passed).
The application printed the CLI usage instructions to `stdout` and returned exit code `1` within a few milliseconds.
Because the process exited immediately, Windows tore down the console window before the user could read anything or type a command. To the user, it appeared as though double-clicking "did nothing."

---

## 3. Architecture & Design of the Fix

### 3.1 Interactive Default Mode (`argc < 2`)
Instead of treating zero arguments as a syntax error that prints usage and exits, `main()` now detects `argc < 2` as a request to launch the interactive SQL shell targeting the engine's default database:

1. **Executable Directory Resolution:**
   Uses the Win32 API `GetModuleFileNameW` to accurately resolve the absolute directory where `klyrdb.exe` resides, handling long paths and Unicode correctly:
   ```cpp
   std::filesystem::path get_executable_directory(const char* argv0);
   ```
2. **Dedicated Data Directory:**
   Targets `<exe_dir>\data\klyrdb.kdb`. The engine automatically creates the `<exe_dir>\data` directory if missing. If filesystem permissions prevent creating a subdirectory, it gracefully falls back to `<exe_dir>\klyrdb.kdb`.
3. **Banner & Connection Notice:**
   Displays clear engine metadata, format version, and the absolute database file path:
   ```text
   ======================================================================
                     KlyrDB Relational Database Engine                   
   ======================================================================
   Engine Version:         0.12.0 (C++20)
   Storage Format Version: 1
   Execution Mode:         Interactive SQL Shell (Default Database)
   Database File:          D:\KlyrDB\build\Release\data\klyrdb.kdb
   ======================================================================
   Type SQL statements ending with ';' or '.help' for help. Type '.quit' to exit.
   ```
4. **Shell Execution:**
   Calls `cmd_sql(default_db_path.string(), "")` which initializes the buffer pool, checks WAL recovery, opens catalogs, and enters `SqlShell::run()`.

### 3.2 Standalone Console Detection & Failure Protection
If an initialization failure occurs when launched via double-click, the console window must not vanish before the user can read the error message. However, CLI scripts and CI pipelines must **never** hang waiting for user input.

To achieve this, `src/main.cpp` implements `is_standalone_console()`:
```cpp
bool is_interactive_console() {
    return (_isatty(_fileno(stdin)) != 0) && (_isatty(_fileno(stdout)) != 0);
}

bool is_standalone_console() {
    if (!is_interactive_console()) {
        return false;
    }
    DWORD pids[2];
    DWORD count = GetConsoleProcessList(pids, 2);
    return (count <= 1);
}
```
- **If stdin/stdout are redirected or piped:** `is_interactive_console()` returns `false`, preventing any pause.
- **If run from CMD / PowerShell / bash:** `GetConsoleProcessList` counts $\ge 2$ processes (the shell and `klyrdb.exe`), so `is_standalone_console()` returns `false`.
- **If double-clicked from Explorer:** Only `klyrdb.exe` is attached to the newly allocated console window (`count == 1`). If an error occurs, `wait_on_exit_if_standalone()` prompts `Press Enter to exit...` so the error remains visible.

### 3.3 Robust Whitespace Trimming in SQL Shell
In `src/sql/sql_shell.cpp`, dot commands (`.quit`, `.exit`, `.help`, `.tables`) previously trimmed only leading whitespace. When input had trailing spaces or CRLF line endings (e.g. from batch scripts or typing `.quit `), the shell previously treated it as unrecognized SQL.
Trimming both leading and trailing whitespace ensures clean, immediate exit across all platforms and input sources.

### 3.4 Convenient Launchers
1. **Root One-Click Launcher (`D:\KlyrDB\Open-KlyrDB.bat`):**
   Added a clean, batch launcher at the repository root that auto-detects `build\Release\klyrdb.exe` (or `build\Debug\klyrdb.exe`) and launches directly into the interactive default shell.
2. **Demo Launcher (`D:\KlyrDB\demo\Open-KlyrDB-Demo.bat`):**
   Maintained for pre-populated demo tables (`users`, `products`, `orders`) targeting `demo\klyrdb_demo.kdb`.

---

## 4. Backward Compatibility Verification

All existing CLI syntax, subcommands, and flags remain 100% backward compatible:

| Command | Syntax | Behavior | Status |
| :--- | :--- | :--- | :--- |
| **No arguments** | `klyrdb.exe` | Launches interactive SQL shell on default database | **NEW / ENHANCED** |
| **Help** | `klyrdb.exe --help` / `-h` / `help` | Prints complete usage and command listing | Verified |
| **Version** | `klyrdb.exe --version` / `-v` / `version` | Displays engine and storage format versions | Verified |
| **Interactive SQL** | `klyrdb.exe sql <file>` | Launches SQL REPL on specified database file | Verified |
| **One-Shot SQL** | `klyrdb.exe sql <file> "<query>"` | Executes single query, prints tabular results, exits | Verified |
| **Create** | `klyrdb.exe create <file>` | Initializes new database file with Page 0 | Verified |
| **Info** | `klyrdb.exe info <file>` | Prints metadata, table catalog, and B+ tree index roots | Verified |
| **Read Page** | `klyrdb.exe read <file> <page_id>` | Displays raw page header and payload preview | Verified |
| **Recover** | `klyrdb.exe recover <file>` | Runs ARIES-style WAL recovery manually | Verified |
| **Checkpoint** | `klyrdb.exe checkpoint <file>` | Flushes dirty pages, writes checkpoint record | Verified |
| **Integrity Check** | `klyrdb.exe integrity-check <file>` | Comprehensive CRC32, B+ tree, and FSM verification | Verified |
| **Stats / Reset** | `klyrdb.exe stats <file>` | Displays buffer pool hits/misses, WAL, and I/O metrics | Verified |
| **Diagnostics** | `klyrdb.exe diagnostics <file> [--json]` | Generates detailed operational diagnostic report | Verified |
| **Analyze** | `klyrdb.exe analyze <file> [table]` | Computes and caches optimizer column statistics | Verified |
| **Physical Backup** | `klyrdb.exe backup <file> <dir> [--overwrite]` | Creates physical snapshot with SHA-256 manifest | Verified |
| **Backup Verify** | `klyrdb.exe backup-verify <dir>` | Validates backup page checksums against manifest | Verified |
| **Restore** | `klyrdb.exe restore <dir> <db> [--force]` | Restores physical backup and verifies integrity | Verified |
| **Dump / Import** | `klyrdb.exe dump` / `import` | Logical schema DDL and record export/import | Verified |
| **CSV Export / Import** | `klyrdb.exe export-csv` / `import-csv` | RFC 4180 CSV export and import into tables | Verified |

---

## 5. Verification & Testing Evidence

### 5.1 CTest Automated Test Suite (Release)
```text
Test project D:/KlyrDB/build
      Start  1: test_page ........................   Passed    0.04 sec
      Start  2: test_page_file ...................   Passed    0.05 sec
      Start  3: test_persistence .................   Passed    0.06 sec
      Start  4: test_buffer_pool .................   Passed    0.09 sec
      Start  5: test_slotted_page ................   Passed    0.03 sec
      Start  6: test_record_manager ..............   Passed    0.09 sec
      Start  7: test_bplus_tree ..................   Passed    0.10 sec
      Start  8: test_fsm .........................   Passed    0.07 sec
      Start  9: test_index_integration ...........   Passed    0.07 sec
      Start 10: test_sql_lexer ...................   Passed    0.03 sec
      Start 11: test_sql_parser ..................   Passed    0.03 sec
      Start 12: test_table_catalog ...............   Passed    0.05 sec
      Start 13: test_sql_executor ................   Passed    0.06 sec
      Start 14: test_wal .........................   Passed    0.07 sec
      Start 15: test_transaction .................   Passed    0.09 sec
      Start 16: test_recovery ....................   Passed    0.19 sec
      Start 17: test_crash_recovery ..............   Passed    0.40 sec
      Start 18: test_constraints .................   Passed    0.09 sec
      Start 19: test_constraint_transactions .....   Passed    0.07 sec
      Start 20: test_query_planner ...............   Passed    0.05 sec
      Start 21: test_explain .....................   Passed    0.06 sec
      Start 22: test_query_plan_execution ........   Passed    0.06 sec
      Start 23: test_query_optimization ..........   Passed    0.06 sec
      Start 24: test_database_locking ............   Passed    5.41 sec
      Start 25: test_process_locking .............   Passed    0.15 sec
      Start 26: test_buffer_pool_concurrency .....   Passed    0.07 sec
      Start 27: test_concurrent_reads ............   Passed    0.05 sec
      Start 28: test_concurrent_writes ...........   Passed    0.05 sec
      Start 29: test_transaction_isolation .......   Passed    0.19 sec
      Start 30: test_checkpoint ..................   Passed    0.16 sec
      Start 31: test_wal_retention ...............   Passed    0.11 sec
      Start 32: test_checkpoint_recovery .........   Passed    0.15 sec
      Start 33: test_integrity_check .............   Passed    0.06 sec
      Start 34: test_storage_corruption ..........   Passed    0.09 sec
      Start 35: test_wal_lifecycle ...............   Passed    0.17 sec
      Start 36: test_physical_backup .............   Passed    0.35 sec
      Start 37: test_backup_verification .........   Passed    0.10 sec
      Start 38: test_backup_corruption ...........   Passed    0.20 sec
      Start 39: test_restore .....................   Passed    0.11 sec
      Start 40: test_restore_failure_safety ......   Passed    0.24 sec
      Start 41: test_logical_export_import .......   Passed    0.16 sec
      Start 42: test_csv_export_import ...........   Passed    0.12 sec
      Start 43: test_backup_crash_recovery .......   Passed    0.14 sec
      Start 44: test_metrics .....................   Passed    0.05 sec
      Start 45: test_query_profiling .............   Passed    0.06 sec
      Start 46: test_randomized_sql ..............   Passed    0.18 sec
      Start 47: test_fuzz_mutations ..............   Passed    0.05 sec
      Start 48: test_reliability_stress ..........   Passed    0.98 sec
      Start 49: test_public_api ..................   Passed    0.08 sec
      Start 50: test_cli_smoke ...................   Passed    1.63 sec
      Start 51: test_adversarial .................   Passed    0.13 sec

100% tests passed, 0 tests failed out of 51
Total Test time (real) = 14.68 sec
```

### 5.2 CTest Automated Test Suite (Debug)
```text
100% tests passed, 0 tests failed out of 51
Total Test time (real) = 15.04 sec
```

### 5.3 Live SQL Session & Persistence Verification
```text
PS D:\KlyrDB> @"
CREATE TABLE products (id INT PRIMARY KEY, name TEXT, price INT);
INSERT INTO products VALUES (1, 'Widget', 100);
INSERT INTO products VALUES (2, 'Gadget', 250);
SELECT * FROM products;
.quit
"@ | D:\KlyrDB\build\Release\klyrdb.exe

======================================================================
                  KlyrDB Relational Database Engine                   
======================================================================
Engine Version:         0.12.0 (C++20)
Storage Format Version: 1
Execution Mode:         Interactive SQL Shell (Default Database)
Database File:          D:\KlyrDB\build\Release\data\klyrdb.kdb
======================================================================
Type SQL statements ending with ';' or '.help' for help. Type '.quit' to exit.

=========================================================
 KlyrDB Interactive SQL Shell (Phase 5 - WAL & Txns)
 Type SQL statements ending with ';' or '.help' for help.
 Type '.quit' to exit.
=========================================================

klyrdb> Table 'products' created.
klyrdb> Rows inserted: 1
klyrdb> Rows inserted: 1
klyrdb> +----+--------+-------+
| id | name   | price |
+----+--------+-------+
| 1  | Widget | 100   |
| 2  | Gadget | 250   |
+----+--------+-------+
2 rows in set.
klyrdb> Bye!
```

### 5.4 Subsequent Session Querying Persisted Data
```text
PS D:\KlyrDB> @"
SELECT * FROM products;
.quit
"@ | D:\KlyrDB\build\Release\klyrdb.exe

======================================================================
                  KlyrDB Relational Database Engine                   
======================================================================
Engine Version:         0.12.0 (C++20)
Storage Format Version: 1
Execution Mode:         Interactive SQL Shell (Default Database)
Database File:          D:\KlyrDB\build\Release\data\klyrdb.kdb
======================================================================
Type SQL statements ending with ';' or '.help' for help. Type '.quit' to exit.

[*] Pending WAL records detected; running crash recovery...
[+] Recovery completed (Redone: 0, Undone: 0)
=========================================================
 KlyrDB Interactive SQL Shell (Phase 5 - WAL & Txns)
 Type SQL statements ending with ';' or '.help' for help.
 Type '.quit' to exit.
=========================================================

klyrdb> +----+--------+-------+
| id | name   | price |
+----+--------+-------+
| 1  | Widget | 100   |
| 2  | Gadget | 250   |
+----+--------+-------+
2 rows in set.
klyrdb> Bye!
```

---

## 6. Safety & Release Boundaries

| Boundary Condition | Verification Result |
| :--- | :--- |
| **Git Tag `v0.12.0` (`a88cd6b`)** | Preserved unmodified |
| **Release Commit (`2d07772`)** | Preserved unmodified |
| **Release Archive (`dist/klyrdb-0.12.0-windows-x64.zip`)** | Preserved unmodified |
| **Demo Database (`demo/klyrdb_demo.kdb`)** | Preserved unmodified |
| **Git Commits & Pushes** | No commits or pushes performed without explicit authorization |
| **Storage / WAL / Execution Internals** | Engine core and storage algorithms preserved intact |

---

## 7. How to Use KlyrDB on Windows

Users now have three convenient, reliable ways to interact with KlyrDB on Windows:

### Method 1: Double-Click the Executable Directly
1. Navigate to `build\Release\` in Windows Explorer.
2. Double-click `klyrdb.exe`.
3. A command window immediately opens with the KlyrDB interactive shell connected to `build\Release\data\klyrdb.kdb`.
4. Run SQL commands ending with `;` or enter `.help`.
5. Type `.quit` to exit cleanly.

### Method 2: Double-Click the Root Launcher
1. Open the repository root folder `D:\KlyrDB` in Windows Explorer.
2. Double-click `Open-KlyrDB.bat`.
3. It launches the compiled Release binary directly into the default interactive shell.

### Method 3: Double-Click the Preloaded Demo Launcher
1. Open `demo\` in Windows Explorer.
2. Double-click `Open-KlyrDB-Demo.bat`.
3. It opens the preloaded demo database (`demo\klyrdb_demo.kdb`) containing the `users`, `products`, and `orders` tables.
