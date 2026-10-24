cmake_minimum_required(VERSION 3.20)
project(KlyrDB VERSION 0.12.0 LANGUAGES CXX)

# Enforce standard C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(MSVC)
    add_compile_options(/FS)
endif()

# Enable testing
include(CTest)

# Storage & SQL Engine Library
add_library(klyrdb_storage STATIC
    src/common/status.cpp
    src/common/checksum.cpp
    src/storage/page_header.cpp
    src/storage/page.cpp
    src/storage/metadata_page.cpp
    src/storage/page_file.cpp
    src/storage/record.cpp
    src/storage/slotted_page.cpp
    src/storage/buffer_pool_manager.cpp
    src/storage/record_manager.cpp
    src/storage/free_space_map.cpp
    src/index/bplus_tree_node.cpp
    src/index/bplus_tree.cpp
    src/index/index_catalog.cpp
    src/catalog/table_catalog.cpp
    src/catalog/table_statistics.cpp
    src/sql/row.cpp
    src/sql/lexer.cpp
    src/sql/parser.cpp
    src/sql/executor.cpp
    src/sql/query_planner.cpp
    src/sql/sql_shell.cpp
    src/wal/log_record.cpp
    src/wal/write_ahead_log.cpp
    src/txn/transaction_manager.cpp
    src/recovery/recovery_manager.cpp
    src/recovery/checkpoint_manager.cpp
    src/storage/integrity_checker.cpp
    src/concurrency/process_file_lock.cpp
    src/concurrency/database_lock.cpp
    src/concurrency/lock_manager.cpp
    src/backup/backup_manifest.cpp
    src/backup/backup_verifier.cpp
    src/backup/backup_manager.cpp
    src/backup/restore_manager.cpp
    src/backup/logical_exporter.cpp
    src/backup/logical_importer.cpp
    src/backup/csv_migrator.cpp
    src/diagnostics/metrics.cpp
    src/api/result.cpp
    src/api/connection.cpp
    src/api/database.cpp
)

add_library(KlyrDB::KlyrDB ALIAS klyrdb_storage)

target_include_directories(klyrdb_storage PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Compiler warnings and options
if(MSVC)
    target_compile_options(klyrdb_storage PRIVATE /W4 /permissive-)
else()
    target_compile_options(klyrdb_storage PRIVATE -Wall -Wextra -Wpedantic)
endif()

# CLI Database Tool
add_executable(klyrdb src/main.cpp)
target_link_libraries(klyrdb PRIVATE klyrdb_storage)
if(MSVC)
    target_compile_options(klyrdb PRIVATE /W4 /permissive-)
else()
    target_compile_options(klyrdb PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Storage Demonstration Program
add_executable(storage_demo examples/storage_demo.cpp)
target_link_libraries(storage_demo PRIVATE klyrdb_storage)
if(MSVC)
    target_compile_options(storage_demo PRIVATE /W4 /permissive-)
else()
    target_compile_options(storage_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

# SQL Demonstration Program
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/sql_demo.cpp")
    add_executable(sql_demo examples/sql_demo.cpp)
    target_link_libraries(sql_demo PRIVATE klyrdb_storage)
    if(MSVC)
        target_compile_options(sql_demo PRIVATE /W4 /permissive-)
    else()
        target_compile_options(sql_demo PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

# WAL & Transaction Demonstration Program
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/wal_demo.cpp")
    add_executable(wal_demo examples/wal_demo.cpp)
    target_link_libraries(wal_demo PRIVATE klyrdb_storage)
    if(MSVC)
        target_compile_options(wal_demo PRIVATE /W4 /permissive-)
    else()
        target_compile_options(wal_demo PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

# Schema Constraints & Relational Integrity Demonstration Program
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/constraints_demo.cpp")
    add_executable(constraints_demo examples/constraints_demo.cpp)
    target_link_libraries(constraints_demo PRIVATE klyrdb_storage)
    if(MSVC)
        target_compile_options(constraints_demo PRIVATE /W4 /permissive-)
    else()
        target_compile_options(constraints_demo PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

# Index & Storage Benchmarking Executable
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/benchmark_index.cpp")
    add_executable(benchmark_index benchmarks/benchmark_index.cpp)
    target_link_libraries(benchmark_index PRIVATE klyrdb_storage)
    if(MSVC)
        target_compile_options(benchmark_index PRIVATE /W4 /permissive-)
    else()
        target_compile_options(benchmark_index PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

# Query Optimizer Benchmarking Executable
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/benchmark_query_optimizer.cpp")
    add_executable(benchmark_query_optimizer benchmarks/benchmark_query_optimizer.cpp)
    target_link_libraries(benchmark_query_optimizer PRIVATE klyrdb_storage)
    if(MSVC)
        target_compile_options(benchmark_query_optimizer PRIVATE /W4 /permissive-)
    else()
        target_compile_options(benchmark_query_optimizer PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

# Concurrency & Locking Benchmarking Executable
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/benchmark_concurrency.cpp")
    add_executable(benchmark_concurrency benchmarks/benchmark_concurrency.cpp)
    target_link_libraries(benchmark_concurrency PRIVATE klyrdb_storage)
    if(MSVC)
        target_compile_options(benchmark_concurrency PRIVATE /W4 /permissive-)
    else()
        target_compile_options(benchmark_concurrency PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

# WAL Lifecycle & Checkpoint Benchmarking Executable
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/benchmark_wal_lifecycle.cpp")
    add_executable(benchmark_wal_lifecycle benchmarks/benchmark_wal_lifecycle.cpp)
    target_link_libraries(benchmark_wal_lifecycle PRIVATE klyrdb_storage)
    if(MSVC)
        target_compile_options(benchmark_wal_lifecycle PRIVATE /W4 /permissive-)
    else()
        target_compile_options(benchmark_wal_lifecycle PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

# Physical Backup, Restore & Logical Migration Benchmarking Executable
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/benchmark_backup_restore.cpp")
    add_executable(benchmark_backup_restore benchmarks/benchmark_backup_restore.cpp)
    target_link_libraries(benchmark_backup_restore PRIVATE klyrdb_storage)
    if(MSVC)
        target_compile_options(benchmark_backup_restore PRIVATE /W4 /permissive-)
    else()
        target_compile_options(benchmark_backup_restore PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

# Reliability & Operational Performance Benchmarking Executable
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/benchmark_reliability.cpp")
    add_executable(benchmark_reliability benchmarks/benchmark_reliability.cpp)
    target_link_libraries(benchmark_reliability PRIVATE klyrdb_storage)
    if(MSVC)
        target_compile_options(benchmark_reliability PRIVATE /W4 /permissive-)
    else()
        target_compile_options(benchmark_reliability PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

# Test Suite
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Installation & Packaging
install(TARGETS klyrdb_storage klyrdb
    EXPORT KlyrDBTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/klyrdb
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)

install(EXPORT KlyrDBTargets
    FILE KlyrDBTargets.cmake
    NAMESPACE KlyrDB::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KlyrDB
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/KlyrDBConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/KlyrDBConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/KlyrDBConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KlyrDB
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/KlyrDBConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/KlyrDBConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KlyrDB
)
