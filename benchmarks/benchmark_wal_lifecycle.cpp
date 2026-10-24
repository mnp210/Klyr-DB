#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/recovery/checkpoint_manager.hpp"
#include "klyrdb/recovery/recovery_manager.hpp"
#include "klyrdb/storage/integrity_checker.hpp"
#include "klyrdb/sql/executor.hpp"

#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <filesystem>

using namespace klyrdb;

struct BenchTimer {
    std::chrono::high_resolution_clock::time_point start{std::chrono::high_resolution_clock::now()};
    void reset() { start = std::chrono::high_resolution_clock::now(); }
    [[nodiscard]] double elapsed_ms() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(now - start).count();
    }
    [[nodiscard]] double elapsed_us() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::micro>(now - start).count();
    }
};

static void print_header(const std::string& title) {
    std::cout << "\n======================================================================\n";
    std::cout << "  " << title << "\n";
    std::cout << "======================================================================\n";
}

int main() {
    std::cout << "======================================================================\n";
    std::cout << "        KLYRDB PHASE 9 WAL LIFECYCLE & CHECKPOINT BENCHMARKS          \n";
    std::cout << "======================================================================\n";

    auto temp_dir = std::filesystem::temp_directory_path();
    auto now_str = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto db_path = temp_dir / ("klyrdb_bench_wal_" + now_str + ".kdb");
    auto wal_path = db_path.string() + ".wal";
    auto lock_path = db_path.string() + ".lock";

    std::error_code ec;
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(wal_path, ec);
    std::filesystem::remove(lock_path, ec);

    print_header("WORKLOAD EXECUTION (Relational Multi-Table + Foreign Keys + Rollbacks)");

    constexpr int NUM_TRANSACTIONS = 100;
    std::cout << "Executing workload: " << NUM_TRANSACTIONS << " multi-operation transactions...\n";

    {
        auto pf = PageFile::create(db_path, false).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 64);
        TransactionManager tm(bpm, *wal);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();

        SqlExecutor executor(bpm, rm, *tc, *ic);
        executor.set_transaction_manager(&tm);

        (void)executor.execute("CREATE TABLE departments (dept_id INTEGER PRIMARY KEY, dept_name TEXT);");
        (void)executor.execute("INSERT INTO departments VALUES (1, 'Engineering');");
        (void)executor.execute("INSERT INTO departments VALUES (2, 'Finance');");
        (void)executor.execute("INSERT INTO departments VALUES (3, 'Operations');");

        (void)executor.execute("CREATE TABLE employees (emp_id INTEGER PRIMARY KEY, name TEXT, salary INTEGER, dept_id INTEGER REFERENCES departments(dept_id));");
        (void)executor.execute("CREATE INDEX idx_salary ON employees(salary);");

        int rollback_count = 0;
        for (int t = 1; t <= NUM_TRANSACTIONS; ++t) {
            (void)executor.execute("BEGIN;");
            int emp_id = t;
            int salary = 50000 + (t * 250);
            int dept_id = (t % 3) + 1;

            std::string ins = "INSERT INTO employees VALUES (" + std::to_string(emp_id) +
                              ", 'employee_" + std::to_string(emp_id) + "', " +
                              std::to_string(salary) + ", " + std::to_string(dept_id) + ");";
            (void)executor.execute(ins);

            if (t > 10 && t % 5 == 0) {
                // Update salary
                std::string upd = "UPDATE employees SET salary = salary + 1000 WHERE emp_id = " + std::to_string(t - 5) + ";";
                (void)executor.execute(upd);
            }

            if (t > 20 && t % 10 == 0) {
                // Delete an older employee
                std::string del = "DELETE FROM employees WHERE emp_id = " + std::to_string(t - 15) + ";";
                (void)executor.execute(del);
            }

            // Periodically test rollbacks
            if (t % 7 == 0) {
                (void)executor.execute("ROLLBACK;");
                rollback_count++;
            } else {
                (void)executor.execute("COMMIT;");
            }
        }
        std::cout << "Workload completed (" << NUM_TRANSACTIONS << " txns, " << rollback_count << " rollbacks).\n";

        size_t wal_size_uncheckpointed = wal->file_size();
        size_t db_size_uncheckpointed = std::filesystem::file_size(db_path, ec);

        std::cout << "Uncheckpointed WAL Size: " << wal_size_uncheckpointed << " bytes\n";
        std::cout << "Database File Size:      " << db_size_uncheckpointed << " bytes\n";

        // ---------------------------------------------------------------------
        // Measure Checkpoint Operation
        // ---------------------------------------------------------------------
        print_header("CHECKPOINT & WAL TRUNCATION MEASUREMENT");
        CheckpointManager chk_mgr(bpm, *wal, tm);
        auto chk_res = chk_mgr.checkpoint(CheckpointMode::Safe);
        if (chk_res.is_err()) {
            std::cerr << "Checkpoint failed: " << chk_res.status().message() << "\n";
            return 1;
        }

        const auto& cs = chk_res.value();
        std::cout << "Checkpoint Duration:     " << std::fixed << std::setprecision(2) << cs.duration_ms << " ms\n";
        std::cout << "Checkpoint LSN:          " << cs.checkpoint_lsn << "\n";
        std::cout << "Min Required LSN:        " << cs.min_required_lsn << "\n";
        std::cout << "WAL Size Before Checkpoint: " << cs.wal_bytes_before << " bytes\n";
        std::cout << "WAL Size After Checkpoint:  " << cs.wal_bytes_after << " bytes\n";
        double reduction = (1.0 - (double)cs.wal_bytes_after / (double)cs.wal_bytes_before) * 100.0;
        std::cout << "WAL Storage Reduction:   " << std::fixed << std::setprecision(1) << reduction << " %\n";
    }

    // -------------------------------------------------------------------------
    // Measure Recovery Workload
    // -------------------------------------------------------------------------
    print_header("POST-CHECKPOINT STARTUP RECOVERY MEASUREMENT");
    {
        auto pf = PageFile::open(db_path, false).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 64);

        RecoveryManager rm(bpm, *wal);
        BenchTimer rec_timer;
        auto rec_res = rm.recover();
        double rec_ms = rec_timer.elapsed_ms();

        if (rec_res.is_ok()) {
            const auto& rs = rec_res.value();
            std::cout << "Recovery Execution Time: " << std::fixed << std::setprecision(3) << rec_ms << " ms\n";
            std::cout << "Total Log Records in WAL: " << rs.total_log_records << "\n";
            std::cout << "Log Records Scanned:     " << rs.records_scanned << "\n";
            std::cout << "Pages Redone:            " << rs.pages_redone << "\n";
            std::cout << "Transactions Undone:     " << rs.uncommitted_txns << "\n";
        }
    }

    // -------------------------------------------------------------------------
    // Storage & Index Integrity Audit
    // -------------------------------------------------------------------------
    print_header("STORAGE & RELATIONAL INTEGRITY VERIFICATION");
    {
        auto pf = PageFile::open(db_path, false).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        IntegrityChecker checker(*pf, wal.get());
        auto rep_res = checker.check_all();
        if (rep_res.is_ok()) {
            rep_res.value().print(std::cout);
        }
    }

    // Cleanup
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(wal_path, ec);
    std::filesystem::remove(lock_path, ec);

    print_header("BENCHMARK COMPLETED SUCCESSFULLY");
    return 0;
}
