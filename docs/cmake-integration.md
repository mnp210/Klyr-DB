# Integrating KlyrDB via CMake

## 1. Overview

External C++20 projects consume KlyrDB using standard CMake `find_package(KlyrDB REQUIRED)` and link against the exported target `KlyrDB::KlyrDB`.

---

## 2. Sample `CMakeLists.txt` for Consumer Application

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_klyrdb_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 1. Locate installed KlyrDB package
find_package(KlyrDB REQUIRED)

# 2. Add your application executable
add_executable(my_klyrdb_app main.cpp)

# 3. Link against KlyrDB
target_link_libraries(my_klyrdb_app PRIVATE KlyrDB::KlyrDB)
```

---

## 3. Configuring the Consumer Project

If KlyrDB was installed to a non-standard prefix (e.g. `C:/opt/klyrdb` or `D:/KlyrDB/build/install_test`), pass `CMAKE_PREFIX_PATH`:

```powershell
cmake -B build -S . -DCMAKE_PREFIX_PATH="C:/opt/klyrdb" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The exported imported target `KlyrDB::KlyrDB` automatically configures:
- Public include directories (`<prefix>/include`).
- Required C++20 standard compile features.
- Library link dependencies.
