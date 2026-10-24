#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/diagnostics/metrics.hpp"
#include "klyrdb/catalog/table_statistics.hpp"
#include "klyrdb/sql/executor.hpp"

#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <numeric>

using namespace klyrdb;

struct LatencyStats {
    double p50_us{0.0};
    double p95_us{0.0};
    double p99_us{0.0};
    double mean_us{0.0};
    double throughput_ops_sec{0.0};
};

static LatencyStats compute_stats(std::vector<double>& latencies_us, double total_elapsed_ms) {
    if (latencies_us.empty()) return {};
    std::sort(latencies_us.begin(), latencies_us.end());

    LatencyStats s;
    size_t n = latencies_us.size();
    s.p50_us = latencies_us[n * 50 / 100];
    s.p95_us = latencies_us[n * 95 / 100];
    s.p99_us = latencies_us[n * 99 / 100];

    double sum = std::accumulate(latencies_us.begin(), latencies_us.end(), 0.0);
    s.mean_us = sum / static_cast<double>(n);

    if (total_elapsed_ms > 0.0) {
        s.throughput_ops_sec = (static_cast<double>(n) / (total_elapsed_ms / 1000.0));
    }
    return s;
}

static void print_benchmark_row(std::string_view workload, const LatencyStats& s, double hit_ratio) {
    std::cout << std::left << std::setw(28) << workload
              << std::right << std::setw(12) << std::fixed << std::setprecision(1) << s.throughput_ops_sec
              << std::setw(11) << std::fixed << std::setprecision(1) << s.p50_us
              << std::setw(11) << std::fixed << std::setprecision(1) << s.p95_us
              << std::setw(11) << std::fixed << std::setprecision(1) << s.p99_us
              << std::setw(12) << std::fixed << std::setprecision(2) << (hit_ratio * 100.0) << "%\n";
}

int main() {
    std::cout << "========================================================================================\n";
    std::cout << "         KLYRDB PHASE 11 RELIABILITY & OPERATIONAL PERFORMANCE BENCHMARKS               \n";
    std::cout << "========================================================================================\n\n";

    auto temp_dir = std::filesystem::temp_directory_path();
    auto db_path = temp_dir / ("klyrdb_bench_rel_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".kdb");
    auto wal_path = db_path.string() + ".wal";

    std::error_code ec;
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(wal_path, ec);

    auto pf_res = PageFile::create(db_path);
    if (!pf_res.is_ok()) {
        std::cerr << "Failed to create benchmark database: " << pf_res.status().to_string() << "\n";
        return 1;
    }
    auto pf = std::move(pf_res.value());

    auto wal_res = WriteAheadLog::open_or_create(wal_path);
    if (!wal_res.is_ok()) {
        std::cerr << "Failed to create benchmark WAL: " << wal_res.status().to_string() << "\n";
        return 1;
    }
    auto wal = std::move(wal_res.value());

    BufferPoolManager bpm(*pf, 256);
    auto metrics = DatabaseMetricsRegistry::instance().get_metrics(db_path);
    metrics->reset();
    bpm.set_metrics(metrics.get());
    bpm.set_wal(wal.get());

    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();
    TransactionManager tm(bpm, *wal);

    SqlExecutor executor(bpm, rm, *tc, *ic);
    executor.set_transaction_manager(&tm);

    (void)executor.execute("CREATE TABLE accounts (id INTEGER PRIMARY KEY, balance INTEGER, region TEXT);");
    (void)executor.execute("CREATE INDEX idx_balance ON accounts(balance);");

    std::cout << std::left << std::setw(28) << "Workload Operation"
              << std::right << std::setw(12) << "Throughput"
              << std::setw(11) << "P50 (us)"
              << std::setw(11) << "P95 (us)"
              << std::setw(11) << "P99 (us)"
              << std::setw(14) << "Cache Hit %" << "\n";
    std::cout << std::string(88, '-') << "\n";

    // 1. Workload: 1,000 Sequential Inserts
    {
        constexpr int N = 1000;
        std::vector<double> latencies;
        latencies.reserve(N);

        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 1; i <= N; ++i) {
            auto op_start = std::chrono::high_resolution_clock::now();
            std::string sql = "INSERT INTO accounts VALUES (" + std::to_string(i) + ", " +
                              std::to_string(i * 5) + ", 'US-EAST');";
            (void)executor.execute(sql);
            auto op_end = std::chrono::high_resolution_clock::now();
            latencies.push_back(std::chrono::duration<double, std::micro>(op_end - op_start).count());
        }
        auto end = std::chrono::high_resolution_clock::now();
        double total_ms = std::chrono::duration<double, std::milli>(end - start).count();

        auto stats = compute_stats(latencies, total_ms);
        print_benchmark_row("1k Sequential Inserts", stats, metrics->snapshot().buffer_pool.hit_ratio());
    }

    // 2. Workload: 1,000 Primary Key Seeks (IndexSeek)
    {
        constexpr int N = 1000;
        std::vector<double> latencies;
        latencies.reserve(N);

        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 1; i <= N; ++i) {
            auto op_start = std::chrono::high_resolution_clock::now();
            std::string sql = "SELECT * FROM accounts WHERE id = " + std::to_string((i % 1000) + 1) + ";";
            (void)executor.execute(sql);
            auto op_end = std::chrono::high_resolution_clock::now();
            latencies.push_back(std::chrono::duration<double, std::micro>(op_end - op_start).count());
        }
        auto end = std::chrono::high_resolution_clock::now();
        double total_ms = std::chrono::duration<double, std::milli>(end - start).count();

        auto stats = compute_stats(latencies, total_ms);
        print_benchmark_row("1k PK Point Seeks", stats, metrics->snapshot().buffer_pool.hit_ratio());
    }

    // 3. Workload: 500 Range Scans
    {
        constexpr int N = 500;
        std::vector<double> latencies;
        latencies.reserve(N);

        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 1; i <= N; ++i) {
            int low = (i * 7) % 4000;
            int high = low + 200;
            auto op_start = std::chrono::high_resolution_clock::now();
            std::string sql = "SELECT * FROM accounts WHERE balance >= " + std::to_string(low) +
                              " AND balance <= " + std::to_string(high) + ";";
            (void)executor.execute(sql);
            auto op_end = std::chrono::high_resolution_clock::now();
            latencies.push_back(std::chrono::duration<double, std::micro>(op_end - op_start).count());
        }
        auto end = std::chrono::high_resolution_clock::now();
        double total_ms = std::chrono::duration<double, std::milli>(end - start).count();

        auto stats = compute_stats(latencies, total_ms);
        print_benchmark_row("500 Index Range Scans", stats, metrics->snapshot().buffer_pool.hit_ratio());
    }

    // 4. Workload: 1,000 Point Updates
    {
        constexpr int N = 1000;
        std::vector<double> latencies;
        latencies.reserve(N);

        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 1; i <= N; ++i) {
            auto op_start = std::chrono::high_resolution_clock::now();
            std::string sql = "UPDATE accounts SET balance = " + std::to_string(i * 10) +
                              " WHERE id = " + std::to_string((i % 1000) + 1) + ";";
            (void)executor.execute(sql);
            auto op_end = std::chrono::high_resolution_clock::now();
            latencies.push_back(std::chrono::duration<double, std::micro>(op_end - op_start).count());
        }
        auto end = std::chrono::high_resolution_clock::now();
        double total_ms = std::chrono::duration<double, std::milli>(end - start).count();

        auto stats = compute_stats(latencies, total_ms);
        print_benchmark_row("1k Point Updates", stats, metrics->snapshot().buffer_pool.hit_ratio());
    }

    // 5. Run ANALYZE and benchmark statistics gathering
    {
        auto start = std::chrono::high_resolution_clock::now();
        (void)executor.execute("ANALYZE accounts;");
        auto end = std::chrono::high_resolution_clock::now();
        double dur_ms = std::chrono::duration<double, std::milli>(end - start).count();
        std::cout << "\n[+] Full table ANALYZE completed in " << std::fixed << std::setprecision(2) << dur_ms << " ms\n";
    }

    std::cout << std::string(88, '-') << "\n";
    std::cout << "\nCumulative Database Metrics:\n" << metrics->snapshot().to_string();

    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(wal_path, ec);
    std::filesystem::remove(db_path.string() + ".lock", ec);

    return 0;
}
