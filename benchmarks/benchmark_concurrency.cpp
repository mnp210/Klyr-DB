#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/sql/executor.hpp"

#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <filesystem>
#include <thread>
#include <atomic>
#include <algorithm>
#include <numeric>

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
    [[nodiscard]] double elapsed_sec() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double>(now - start).count();
    }
};

static void print_header(const std::string& title) {
    std::cout << "\n======================================================================\n";
    std::cout << "  " << title << "\n";
    std::cout << "======================================================================\n";
}

int main() {
    std::cout << "======================================================================\n";
    std::cout << "          KLYRDB PHASE 8 CONCURRENCY & LOCKING BENCHMARKS             \n";
    std::cout << "======================================================================\n";

    auto temp_dir = std::filesystem::temp_directory_path();
    auto db_path = temp_dir / ("klyrdb_bench_concurrency_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".kdb");
    auto lock_path = db_path.string() + ".lock";
    auto wal_path = db_path.string() + ".wal";

    std::error_code ec;
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(lock_path, ec);
    std::filesystem::remove(wal_path, ec);

    constexpr int INITIAL_ROWS = 1000;
    std::cout << "\nSetting up dataset: " << INITIAL_ROWS << " rows in table 'accounts'...\n";

    auto pf_res = PageFile::create(db_path, false);
    if (pf_res.is_err()) {
        std::cerr << "Failed to create database: " << pf_res.status().message() << "\n";
        return 1;
    }
    auto pf = std::move(pf_res.value());

    auto wal_res = WriteAheadLog::open_or_create(wal_path);
    if (wal_res.is_err()) {
        std::cerr << "Failed to open WAL: " << wal_res.status().message() << "\n";
        return 1;
    }
    auto wal = std::move(wal_res.value());

    BufferPoolManager bpm(*pf, 128);
    TransactionManager tm(bpm, *wal);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();

    SqlExecutor executor(bpm, rm, *tc, *ic);
    executor.set_transaction_manager(&tm);

    (void)executor.execute("CREATE TABLE accounts (id INTEGER PRIMARY KEY, name TEXT, balance INTEGER);");
    (void)executor.execute("CREATE INDEX idx_balance ON accounts(balance);");

    for (int i = 1; i <= INITIAL_ROWS; ++i) {
        std::string sql = "INSERT INTO accounts VALUES (" + std::to_string(i) + ", 'client_" +
                          std::to_string(i) + "', " + std::to_string((i * 17) % 50000) + ");";
        (void)executor.execute(sql);
    }
    std::cout << "Setup completed.\n";

    // -------------------------------------------------------------------------
    // Benchmark 1: Concurrent Read Scaling (1, 2, 4, 8 threads)
    // -------------------------------------------------------------------------
    print_header("BENCHMARK 1: CONCURRENT READ SCALING (SWMR Shared Locks)");
    std::cout << std::left << std::setw(12) << "Threads"
              << std::setw(16) << "Total Reads"
              << std::setw(16) << "Time (ms)"
              << std::setw(18) << "Throughput (qps)"
              << std::setw(14) << "Speedup" << "\n";
    std::cout << std::string(76, '-') << "\n";

    double single_thread_qps = 0.0;
    const std::vector<size_t> thread_counts = {1, 2, 4, 8};
    constexpr size_t QUERIES_PER_THREAD = 250;

    for (size_t threads : thread_counts) {
        std::vector<std::thread> workers;
        std::atomic<size_t> completed_queries{0};

        BenchTimer timer;
        for (size_t t = 0; t < threads; ++t) {
            workers.emplace_back([&, t]() {
                for (size_t q = 0; q < QUERIES_PER_THREAD; ++q) {
                    int target_id = static_cast<int>((q * 7 + t) % INITIAL_ROWS + 1);
                    std::string sql = "SELECT id, name, balance FROM accounts WHERE id = " + std::to_string(target_id) + ";";
                    auto res = executor.execute(sql);
                    if (res.is_ok() && res.value().row_count() == 1) {
                        completed_queries.fetch_add(1);
                    }
                }
            });
        }

        for (auto& w : workers) {
            w.join();
        }

        double elapsed_ms = timer.elapsed_ms();
        double qps = (completed_queries.load() / (elapsed_ms / 1000.0));
        if (threads == 1) {
            single_thread_qps = qps;
        }
        double speedup = single_thread_qps > 0 ? (qps / single_thread_qps) : 1.0;

        std::cout << std::left << std::setw(12) << threads
                  << std::setw(16) << completed_queries.load()
                  << std::setw(16) << std::fixed << std::setprecision(2) << elapsed_ms
                  << std::setw(18) << std::fixed << std::setprecision(1) << qps
                  << std::setw(14) << std::fixed << std::setprecision(2) << (std::to_string(speedup) + "x")
                  << "\n";
    }

    // -------------------------------------------------------------------------
    // Benchmark 2: Concurrent Writers Under Contention (Serialized Execution)
    // -------------------------------------------------------------------------
    print_header("BENCHMARK 2: CONCURRENT WRITERS UNDER CONTENTION");
    std::cout << std::left << std::setw(12) << "Writers"
              << std::setw(16) << "Total Inserts"
              << std::setw(16) << "Time (ms)"
              << std::setw(18) << "Throughput (ops)"
              << std::setw(16) << "Avg Latency (ms)" << "\n";
    std::cout << std::string(78, '-') << "\n";

    const std::vector<size_t> writer_counts = {1, 2, 4};
    constexpr int INSERTS_PER_WRITER = 100;
    int base_id = 10000;

    for (size_t writers : writer_counts) {
        std::vector<std::thread> worker_threads;
        std::atomic<size_t> completed_writes{0};

        BenchTimer timer;
        for (size_t w = 0; w < writers; ++w) {
            worker_threads.emplace_back([&, w]() {
                int start_id = base_id + static_cast<int>(w * INSERTS_PER_WRITER);
                for (int i = 0; i < INSERTS_PER_WRITER; ++i) {
                    int cur_id = start_id + i;
                    std::string sql = "INSERT INTO accounts VALUES (" + std::to_string(cur_id) +
                                      ", 'new_client', 500);";
                    auto res = executor.execute(sql);
                    if (res.is_ok()) {
                        completed_writes.fetch_add(1);
                    }
                }
            });
        }

        for (auto& wt : worker_threads) {
            wt.join();
        }

        double elapsed_ms = timer.elapsed_ms();
        double wops = (completed_writes.load() / (elapsed_ms / 1000.0));
        double avg_lat = elapsed_ms / completed_writes.load();

        std::cout << std::left << std::setw(12) << writers
                  << std::setw(16) << completed_writes.load()
                  << std::setw(16) << std::fixed << std::setprecision(2) << elapsed_ms
                  << std::setw(18) << std::fixed << std::setprecision(1) << wops
                  << std::setw(16) << std::fixed << std::setprecision(3) << avg_lat
                  << "\n";

        base_id += static_cast<int>(writers * INSERTS_PER_WRITER + 100);
    }

    // -------------------------------------------------------------------------
    // Benchmark 3: Mixed Workload (Concurrent Readers with Active Writer)
    // -------------------------------------------------------------------------
    print_header("BENCHMARK 3: MIXED WORKLOAD (4 READERS + 1 ACTIVE WRITER)");

    std::atomic<bool> stop_workload{false};
    std::atomic<size_t> mixed_reads{0};
    std::atomic<size_t> mixed_writes{0};
    std::vector<double> read_latencies_us;
    std::mutex lat_mutex;

    // Launch 1 writer thread
    std::thread writer_thread([&]() {
        int writer_id = 90000;
        while (!stop_workload.load()) {
            std::string sql = "INSERT INTO accounts VALUES (" + std::to_string(writer_id++) + ", 'mixed_w', 999);";
            if (executor.execute(sql).is_ok()) {
                mixed_writes.fetch_add(1);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    // Launch 4 reader threads
    constexpr size_t MIXED_READERS = 4;
    std::vector<std::thread> reader_threads;
    BenchTimer mixed_timer;

    for (size_t r = 0; r < MIXED_READERS; ++r) {
        reader_threads.emplace_back([&, r]() {
            std::vector<double> local_lats;
            while (!stop_workload.load() && local_lats.size() < 300) {
                int target_id = static_cast<int>((local_lats.size() * 13 + r) % INITIAL_ROWS + 1);
                std::string sql = "SELECT id, balance FROM accounts WHERE id = " + std::to_string(target_id) + ";";

                BenchTimer qtimer;
                auto res = executor.execute(sql);
                double us = qtimer.elapsed_us();

                if (res.is_ok()) {
                    local_lats.push_back(us);
                    mixed_reads.fetch_add(1);
                }
            }
            std::lock_guard<std::mutex> lock(lat_mutex);
            read_latencies_us.insert(read_latencies_us.end(), local_lats.begin(), local_lats.end());
        });
    }

    for (auto& rt : reader_threads) {
        rt.join();
    }
    stop_workload.store(true);
    writer_thread.join();

    double mixed_elapsed_ms = mixed_timer.elapsed_ms();
    std::sort(read_latencies_us.begin(), read_latencies_us.end());

    double p50_us = read_latencies_us.empty() ? 0.0 : read_latencies_us[read_latencies_us.size() / 2];
    double p95_us = read_latencies_us.empty() ? 0.0 : read_latencies_us[static_cast<size_t>(read_latencies_us.size() * 0.95)];
    double p99_us = read_latencies_us.empty() ? 0.0 : read_latencies_us[static_cast<size_t>(read_latencies_us.size() * 0.99)];

    std::cout << "Mixed Duration:        " << std::fixed << std::setprecision(2) << mixed_elapsed_ms << " ms\n";
    std::cout << "Completed Reads:       " << mixed_reads.load() << " queries\n";
    std::cout << "Completed Writes:      " << mixed_writes.load() << " transactions\n";
    std::cout << "Read Latency p50:      " << std::fixed << std::setprecision(1) << p50_us << " us (" << (p50_us / 1000.0) << " ms)\n";
    std::cout << "Read Latency p95:      " << std::fixed << std::setprecision(1) << p95_us << " us (" << (p95_us / 1000.0) << " ms)\n";
    std::cout << "Read Latency p99:      " << std::fixed << std::setprecision(1) << p99_us << " us (" << (p99_us / 1000.0) << " ms)\n";

    print_header("BENCHMARK COMPLETE");

    // Clean up temporary database files
    pf->close();
    wal->close();
    pf.reset();
    wal.reset();
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(lock_path, ec);
    std::filesystem::remove(wal_path, ec);

    return 0;
}
