#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
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
    std::cout << "           KLYRDB PHASE 7 QUERY OPTIMIZER BENCHMARKS                  \n";
    std::cout << "======================================================================\n";

    auto temp_dir = std::filesystem::temp_directory_path();
    auto db_path = temp_dir / ("klyrdb_bench_opt_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".kdb");

    std::error_code ec;
    std::filesystem::remove(db_path, ec);

    constexpr int NUM_ROWS = 2000;
    std::cout << "\nSetting up dataset: " << NUM_ROWS << " rows in table 'benchmark_users'...\n";

    auto pf_res = PageFile::create(db_path);
    if (!pf_res.is_ok()) {
        std::cerr << "Failed to create benchmark database: " << pf_res.status().message() << "\n";
        return 1;
    }

    auto pf = std::move(pf_res.value());
    BufferPoolManager bpm(*pf, 128);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();
    SqlExecutor executor(bpm, rm, *tc, *ic);

    // 1. Create table with PRIMARY KEY on id, secondary index on score, unindexed tag
    auto ct_res = executor.execute("CREATE TABLE benchmark_users (id INTEGER PRIMARY KEY, score INTEGER, tag TEXT);");
    if (!ct_res.is_ok()) {
        std::cerr << "CREATE TABLE failed: " << ct_res.status().message() << "\n";
        return 1;
    }
    auto ci_res = executor.execute("CREATE INDEX idx_score ON benchmark_users(score);");
    if (!ci_res.is_ok()) {
        std::cerr << "CREATE INDEX failed: " << ci_res.status().message() << "\n";
        return 1;
    }

    // Populate dataset
    BenchTimer timer;
    timer.reset();
    for (int i = 1; i <= NUM_ROWS; ++i) {
        int score = (i * 7) % 1000;
        std::string tag = (i % 2 == 0) ? "even" : "odd";
        std::string ins = "INSERT INTO benchmark_users VALUES (" + std::to_string(i) + ", " +
                          std::to_string(score) + ", '" + tag + "');";
        auto res = executor.execute(ins);
        if (!res.is_ok()) {
            std::cerr << "Insert failed at row " << i << ": " << res.status().message() << "\n";
            return 1;
        }
    }
    double load_time_ms = timer.elapsed_ms();
    std::cout << "Loaded " << NUM_ROWS << " rows in " << std::fixed << std::setprecision(2) << load_time_ms << " ms.\n";

    // Benchmark 1: Equality Point Lookup
    print_header("Benchmark 1: Point Equality Lookup (Indexed id vs Unindexed tag)");
    {
        // Explain plans
        auto expl_idx = executor.execute("EXPLAIN SELECT * FROM benchmark_users WHERE id = 1234;");
        std::cout << "Indexed Query Plan:\n" << expl_idx.value().rows()[0].get(0).as_text() << "\n";

        auto expl_scan = executor.execute("EXPLAIN SELECT * FROM benchmark_users WHERE tag = 'odd';");
        std::cout << "Table Scan Query Plan:\n" << expl_scan.value().rows()[0].get(0).as_text() << "\n";

        // Measure Indexed Seek
        constexpr int RUNS = 500;
        timer.reset();
        for (int r = 0; r < RUNS; ++r) {
            int target = 1 + (r % NUM_ROWS);
            auto res = executor.execute("SELECT * FROM benchmark_users WHERE id = " + std::to_string(target) + ";");
            if (!res.is_ok() || res.value().row_count() != 1) {
                std::cerr << "Indexed query error\n";
            }
        }
        double idx_us = timer.elapsed_us() / RUNS;

        // Measure Table Scan
        timer.reset();
        for (int r = 0; r < 20; ++r) {
            auto res = executor.execute("SELECT * FROM benchmark_users WHERE tag = 'none';");
            if (!res.is_ok()) {
                std::cerr << "Scan query error\n";
            }
        }
        double scan_us = timer.elapsed_us() / 20;

        std::cout << "Results for Point Lookup (" << NUM_ROWS << " rows):\n";
        std::cout << "  - Index Seek Avg Time:  " << std::setw(8) << idx_us << " us / query\n";
        std::cout << "  - Table Scan Avg Time:  " << std::setw(8) << scan_us << " us / query\n";
        std::cout << "  - Speedup Factor:       " << std::setw(8) << (scan_us / idx_us) << "x faster\n";
    }

    // Benchmark 2: Range Scan
    print_header("Benchmark 2: Range Scan (Selective Index Range Scan vs Full Scan)");
    {
        auto expl_range = executor.execute("EXPLAIN SELECT * FROM benchmark_users WHERE score >= 100 AND score <= 120;");
        std::cout << "Range Scan Plan:\n" << expl_range.value().rows()[0].get(0).as_text() << "\n";

        constexpr int RUNS = 200;
        timer.reset();
        for (int r = 0; r < RUNS; ++r) {
            auto res = executor.execute("SELECT * FROM benchmark_users WHERE score >= 100 AND score <= 120;");
            if (!res.is_ok()) {
                std::cerr << "Range query error\n";
            }
        }
        double range_us = timer.elapsed_us() / RUNS;

        timer.reset();
        for (int r = 0; r < 20; ++r) {
            auto res = executor.execute("SELECT * FROM benchmark_users WHERE tag = 'even';");
            if (!res.is_ok()) {
                std::cerr << "Scan query error\n";
            }
        }
        double scan_us = timer.elapsed_us() / 20;

        std::cout << "Results for Range Scan (" << NUM_ROWS << " rows):\n";
        std::cout << "  - Index Range Scan Avg: " << std::setw(8) << range_us << " us / query\n";
        std::cout << "  - Full Scan Avg:        " << std::setw(8) << scan_us << " us / query\n";
        std::cout << "  - Speedup Factor:       " << std::setw(8) << (scan_us / range_us) << "x faster\n";
    }

    // Benchmark 3: ORDER BY + LIMIT Pushdown
    print_header("Benchmark 3: ORDER BY + LIMIT Pushdown Optimization");
    {
        auto expl_pushed = executor.execute("EXPLAIN SELECT id FROM benchmark_users ORDER BY id ASC LIMIT 5;");
        std::cout << "Optimized Ordered Plan (Index + Early Exit Limit):\n" << expl_pushed.value().rows()[0].get(0).as_text() << "\n";

        auto expl_unpushed = executor.execute("EXPLAIN SELECT id FROM benchmark_users ORDER BY tag ASC LIMIT 5;");
        std::cout << "Unoptimized Ordered Plan (Full Scan + Explicit Sort + Post-sort Limit):\n" << expl_unpushed.value().rows()[0].get(0).as_text() << "\n";

        constexpr int RUNS = 200;
        timer.reset();
        for (int r = 0; r < RUNS; ++r) {
            auto res = executor.execute("SELECT id FROM benchmark_users ORDER BY id ASC LIMIT 5;");
            if (!res.is_ok() || res.value().row_count() != 5) {
                std::cerr << "Pushed limit error\n";
            }
        }
        double pushed_us = timer.elapsed_us() / RUNS;

        timer.reset();
        for (int r = 0; r < 20; ++r) {
            auto res = executor.execute("SELECT id FROM benchmark_users ORDER BY tag ASC LIMIT 5;");
            if (!res.is_ok() || res.value().row_count() != 5) {
                std::cerr << "Sort limit error\n";
            }
        }
        double unpushed_us = timer.elapsed_us() / 20;

        std::cout << "Results for ORDER BY + LIMIT (" << NUM_ROWS << " rows):\n";
        std::cout << "  - Index + Limit Pushdown Avg: " << std::setw(8) << pushed_us << " us / query\n";
        std::cout << "  - Full Scan + In-Memory Sort: " << std::setw(8) << unpushed_us << " us / query\n";
        std::cout << "  - Speedup Factor:             " << std::setw(8) << (unpushed_us / pushed_us) << "x faster\n";
    }

    std::filesystem::remove(db_path, ec);
    std::cout << "\n======================================================================\n";
    std::cout << "All Phase 7 Query Optimizer benchmarks completed successfully.\n";
    std::cout << "======================================================================\n";

    return 0;
}
