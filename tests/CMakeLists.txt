cmake_minimum_required(VERSION 3.20)

enable_testing()

# Helper function to define a test executable
function(klyrdb_add_test test_name test_source)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE klyrdb_storage)
    target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(MSVC)
        target_compile_options(${test_name} PRIVATE /W4 /permissive- /FS)
    else()
        target_compile_options(${test_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

# Phase 1 Tests
klyrdb_add_test(test_page test_page.cpp)
klyrdb_add_test(test_page_file test_page_file.cpp)
klyrdb_add_test(test_persistence test_persistence.cpp)

# Phase 2 Tests
klyrdb_add_test(test_buffer_pool test_buffer_pool.cpp)
klyrdb_add_test(test_slotted_page test_slotted_page.cpp)
klyrdb_add_test(test_record_manager test_record_manager.cpp)

# Phase 3 Tests
klyrdb_add_test(test_bplus_tree test_bplus_tree.cpp)
klyrdb_add_test(test_fsm test_fsm.cpp)
klyrdb_add_test(test_index_integration test_index_integration.cpp)

# Phase 4 Tests
klyrdb_add_test(test_sql_lexer test_sql_lexer.cpp)
klyrdb_add_test(test_sql_parser test_sql_parser.cpp)
klyrdb_add_test(test_table_catalog test_table_catalog.cpp)
klyrdb_add_test(test_sql_executor test_sql_executor.cpp)

# Phase 5 Tests: WAL, Transactions & Crash Recovery
klyrdb_add_test(test_wal test_wal.cpp)
klyrdb_add_test(test_transaction test_transaction.cpp)
klyrdb_add_test(test_recovery test_recovery.cpp)
klyrdb_add_test(test_crash_recovery test_crash_recovery.cpp)

# Phase 6 Tests: Schema Constraints & Relational Integrity
klyrdb_add_test(test_constraints test_constraints.cpp)
klyrdb_add_test(test_constraint_transactions test_constraint_transactions.cpp)

# Phase 7 Tests: Query Planning, Optimization & EXPLAIN
klyrdb_add_test(test_query_planner test_query_planner.cpp)
klyrdb_add_test(test_explain test_explain.cpp)
klyrdb_add_test(test_query_plan_execution test_query_plan_execution.cpp)
klyrdb_add_test(test_query_optimization test_query_optimization.cpp)

# Phase 8 Tests: Concurrency Control, Locking & Database Hardening
klyrdb_add_test(test_database_locking test_database_locking.cpp)
klyrdb_add_test(test_process_locking test_process_locking.cpp)
klyrdb_add_test(test_buffer_pool_concurrency test_buffer_pool_concurrency.cpp)
klyrdb_add_test(test_concurrent_reads test_concurrent_reads.cpp)
klyrdb_add_test(test_concurrent_writes test_concurrent_writes.cpp)
klyrdb_add_test(test_transaction_isolation test_transaction_isolation.cpp)

# Phase 9 Tests: Checkpointing, WAL Lifecycle & Storage Reliability
klyrdb_add_test(test_checkpoint test_checkpoint.cpp)
klyrdb_add_test(test_wal_retention test_wal_retention.cpp)
klyrdb_add_test(test_checkpoint_recovery test_checkpoint_recovery.cpp)
klyrdb_add_test(test_integrity_check test_integrity_check.cpp)
klyrdb_add_test(test_storage_corruption test_storage_corruption.cpp)
klyrdb_add_test(test_wal_lifecycle test_wal_lifecycle.cpp)

# Phase 10 Tests: Physical Backup, Restore, Export/Import & Disaster Recovery
klyrdb_add_test(test_physical_backup test_physical_backup.cpp)
klyrdb_add_test(test_backup_verification test_backup_verification.cpp)
klyrdb_add_test(test_backup_corruption test_backup_corruption.cpp)
klyrdb_add_test(test_restore test_restore.cpp)
klyrdb_add_test(test_restore_failure_safety test_restore_failure_safety.cpp)
klyrdb_add_test(test_logical_export_import test_logical_export_import.cpp)
klyrdb_add_test(test_csv_export_import test_csv_export_import.cpp)
klyrdb_add_test(test_backup_crash_recovery test_backup_crash_recovery.cpp)

# Phase 11 Tests: Observability, Query Profiling, Fuzzing & Reliability
klyrdb_add_test(test_metrics test_metrics.cpp)
klyrdb_add_test(test_query_profiling test_query_profiling.cpp)
klyrdb_add_test(test_randomized_sql test_randomized_sql.cpp)
klyrdb_add_test(test_fuzz_mutations test_fuzz_mutations.cpp)
klyrdb_add_test(test_reliability_stress test_reliability_stress.cpp)

# Phase 12 Tests: Release Engineering & Public C++ API
klyrdb_add_test(test_public_api test_public_api.cpp)
klyrdb_add_test(test_cli_smoke test_cli_smoke.cpp)

# Phase 13 Tests: Adversarial Audit & Release Candidate
klyrdb_add_test(test_adversarial test_adversarial.cpp)







