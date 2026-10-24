# Installing KlyrDB

## 1. Installation Overview

KlyrDB uses CMake's `GNUInstallDirs` conventions to install:
- **Binaries**: `bin/klyrdb` (or `bin/klyrdb.exe` on Windows)
- **Static Library**: `lib/klyrdb_storage.a` or `lib/klyrdb_storage.lib`
- **Public Headers**: `include/klyrdb/` (with `klyrdb.hpp`, `database.hpp`, `connection.hpp`, etc.)
- **CMake Config Packages**: `lib/cmake/KlyrDB/` (`KlyrDBConfig.cmake`, `KlyrDBConfigVersion.cmake`, `KlyrDBTargets.cmake`)

---

## 2. Installing to a Custom Prefix

```powershell
# Windows
cmake --install build --config Release --prefix "C:/opt/klyrdb"
```

```bash
# Linux
sudo cmake --install build --config Release --prefix "/usr/local"
```

---

## 3. Verifying Installation

Verify the installed CLI tool:
```bash
klyrdb --version
klyrdb --help
```

Output:
```text
KlyrDB version 0.12.0
Storage Format Version: 1
Backup Format Version:  1
Language Standard:      C++20
```
