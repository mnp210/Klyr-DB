# Building KlyrDB

## 1. Supported Toolchains & Platforms

KlyrDB is built and verified with standard C++20:
- **Windows**: MSVC 19.30+ (Visual Studio 2022 or 2026 Build Tools)
- **Linux**: GCC 11+ or Clang 13+
- **Build System**: CMake 3.20+

KlyrDB has zero external library dependencies.

---

## 2. Quick-Start Build Instructions

### Windows (PowerShell & Visual Studio Build Tools)
```powershell
# 1. Clone or navigate to the repository
cd D:\KlyrDB

# 2. Configure CMake
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON

# 3. Build the engine, CLI, and test suites
cmake --build build --config Release

# 4. Run the full test suite
ctest --test-dir build -C Release --output-on-failure
```

### Linux / POSIX
```bash
# 1. Configure
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON

# 2. Build
cmake --build build --config Release -j$(nproc)

# 3. Test
ctest --test-dir build -C Release --output-on-failure
```

---

## 3. CMake Configuration Options

| Option | Default | Description |
|:---|:---:|:---|
| `CMAKE_BUILD_TYPE` | `Release` | Target build configuration (`Debug`, `Release`, `RelWithDebInfo`) |
| `BUILD_TESTING` | `ON` | Compiles the 50 CTest test suites |
| `CMAKE_INSTALL_PREFIX` | System default | Destination installation directory |
