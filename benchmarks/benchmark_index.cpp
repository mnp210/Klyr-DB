#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/index/key.hpp"
#include "klyrdb/index/bplus_tree.hpp"
#include "klyrdb/index/index_catalog.hpp"

#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <filesystem>
#include <random>

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
    std::cout << "             KLYRDB PHASE 3 INDEX & STORAGE BENCHMARKS               \n";
    std::cout << "======================================================================\n";

    auto temp_dir = std::filesystem::temp_directory_path();
    auto db_path = temp_dir / ("klyrdb_bench_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".kdb");

    std::error_code ec;
    std::filesystem::remove(db_path, ec);

    constexpr int NUM_RECORDS = 5000;
    std::cout << "\nPreparing database dataset: " << NUM_RECORDS << " records...\n";

    {
        auto pf = PageFile::create(db_path).value();
        BufferPoolManager bpm(*pf, 128);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        auto catalog = IndexCatalog::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());

        BPlusTree tree(bpm);
        tree.set_on_root_change([&catalog](page_id_t new_root) {
            catalog->update_root_page("idx_bench_pk", new_root);
        });

        // -------------------------------------------------------------
        // Benchmark 1: Sequential & Random Insertion with Node Splits
        // -------------------------------------------------------------
        print_header("Benchmark 1: B+ Tree Insertion Throughput (with Node Splits)");
        BenchTimer insert_timer;

        // First record to initialize index in catalog
        std::string payload_init = "Record payload data #1";
        auto rid_init = rm.insert_record(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(payload_init.data()), payload_init.size())).value();
        tree.insert(IndexKey(int64_t{1}), rid_init);
        catalog->create_index("idx_bench_pk", KeyType::Int64, tree.root_page_id());

        insert_timer.reset();
        for (int64_t i = 2; i <= NUM_RECORDS; ++i) {
            std::string payload = "Record payload data #" + std::to_string(i);
            auto rid = rm.insert_record(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(payload.data()), payload.size())).value();
            tree.insert(IndexKey(i), rid);
        }
        double insert_time_ms = insert_timer.elapsed_ms();
        double insert_ops_sec = (NUM_RECORDS - 1) / (insert_time_ms / 1000.0);
        double avg_insert_us = (insert_time_ms * 1000.0) / (NUM_RECORDS - 1);

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  Total inserted records: " << NUM_RECORDS << "\n";
        std::cout << "  Total insertion time:   " << insert_time_ms << " ms\n";
        std::cout << "  Throughput:             " << insert_ops_sec << " inserts/sec\n";
        std::cout << "  Average latency:        " << avg_insert_us << " us / record\n";

        // Flush pages to establish clean baseline
        bpm.flush_all_pages();

        // -------------------------------------------------------------
        // Benchmark 2: Point Lookups: B+ Tree Index vs Full Sequential Scan
        // -------------------------------------------------------------
        print_header("Benchmark 2: Point Lookups: B+ Tree Index vs Full Table Scan");
        constexpr int NUM_LOOKUPS = 2000;
        std::mt19937 rng(42);
        std::uniform_int_distribution<int64_t> dist(1, NUM_RECORDS);

        std::vector<int64_t> sample_keys;
        sample_keys.reserve(NUM_LOOKUPS);
        for (int i = 0; i < NUM_LOOKUPS; ++i) {
            sample_keys.push_back(dist(rng));
        }

        // B+ Tree Indexed Point Lookups
        BenchTimer btree_timer;
        size_t btree_found = 0;
        for (int64_t key : sample_keys) {
            auto rid_res = tree.find_one(IndexKey(key));
            if (rid_res.is_ok()) {
                auto data = rm.get_record(rid_res.value());
                if (data.is_ok()) ++btree_found;
            }
        }
        double btree_time_ms = btree_timer.elapsed_ms();
        double btree_qps = NUM_LOOKUPS / (btree_time_ms / 1000.0);
        double btree_avg_us = (btree_time_ms * 1000.0) / NUM_LOOKUPS;

        std::cout << "  [Indexed B+ Tree Point Lookups]\n";
        std::cout << "    Lookups performed:  " << NUM_LOOKUPS << " (Found: " << btree_found << ")\n";
        std::cout << "    Total time:         " << btree_time_ms << " ms\n";
        std::cout << "    Throughput:         " << btree_qps << " queries/sec\n";
        std::cout << "    Average latency:    " << btree_avg_us << " us / query\n\n";

        // Full Sequential Table Scan Baseline
        // Perform 50 sequential scans for comparison
        constexpr int NUM_SCAN_QUERIES = 50;
        BenchTimer scan_timer;
        size_t scan_found = 0;

        for (int q = 0; q < NUM_SCAN_QUERIES; ++q) {
            int64_t target_key = sample_keys[static_cast<size_t>(q)];
            std::string target_pattern = "Record payload data #" + std::to_string(target_key);
            const uint32_t total_pages = bpm.page_file().page_count();
            bool query_found = false;

            for (page_id_t pid = 1; pid < total_pages && !query_found; ++pid) {
                auto guard_res = bpm.fetch_page_guard(pid);
                if (guard_res.is_err()) continue;
                auto& guard = guard_res.value();
                if (guard->page_type() == PageType::Record && SlottedPage::is_slotted_page(*guard)) {
                    SlottedPage slotted(*guard);
                    for (slot_id_t slot = 0; slot < slotted.slot_count(); ++slot) {
                        auto rec_res = slotted.get_record(slot);
                        if (rec_res.is_ok()) {
                            std::string s(reinterpret_cast<const char*>(rec_res.value().data()), rec_res.value().size());
                            if (s == target_pattern) {
                                ++scan_found;
                                query_found = true;
                                break;
                            }
                        }
                    }
                }
            }
        }
        double scan_time_ms = scan_timer.elapsed_ms();
        double scan_qps = NUM_SCAN_QUERIES / (scan_time_ms / 1000.0);
        double scan_avg_us = (scan_time_ms * 1000.0) / NUM_SCAN_QUERIES;

        std::cout << "  [Sequential Table Scan Baseline]\n";
        std::cout << "    Scans performed:    " << NUM_SCAN_QUERIES << " (Found: " << scan_found << ")\n";
        std::cout << "    Total time:         " << scan_time_ms << " ms\n";
        std::cout << "    Throughput:         " << scan_qps << " scans/sec\n";
        std::cout << "    Average latency:    " << scan_avg_us << " us / scan\n\n";

        double speedup = scan_avg_us / btree_avg_us;
        std::cout << "  >>> SPEEDUP: B+ Tree is " << speedup << "x FASTER than sequential scan <<<\n";

        // -------------------------------------------------------------
        // Benchmark 3: Range Scan Performance
        // -------------------------------------------------------------
        print_header("Benchmark 3: Range Scan Performance");
        int intervals[] = {10, 50, 200, 1000};
        for (int span : intervals) {
            BenchTimer range_timer;
            constexpr int RANGE_RUNS = 100;
            size_t total_items = 0;
            for (int r = 0; r < RANGE_RUNS; ++r) {
                int64_t low = 1000 + (r * 10);
                int64_t high = low + span - 1;
                auto res = tree.scan_range(IndexKey(low), IndexKey(high));
                if (res.is_ok()) total_items += res.value().size();
            }
            double r_time = range_timer.elapsed_ms();
            double avg_range_us = (r_time * 1000.0) / RANGE_RUNS;
            std::cout << "  Range size: " << std::setw(4) << span << " items | Time: "
                      << std::setw(7) << r_time << " ms (" << std::setw(6) << avg_range_us
                      << " us / scan) | Total items scanned: " << total_items << "\n";
        }

        // -------------------------------------------------------------
        // Benchmark 4: Free-Space Map (FSM) Allocation vs Linear Search
        // -------------------------------------------------------------
        print_header("Benchmark 4: FSM Allocation vs Linear Page Scan");
        // Measure 1000 allocation candidate searches using FSM
        BenchTimer fsm_timer;
        constexpr int ALLOC_QUERIES = 2000;
        size_t fsm_found = 0;
        for (int i = 0; i < ALLOC_QUERIES; ++i) {
            auto page_res = fsm->find_page_with_space(64);
            if (page_res.is_ok()) ++fsm_found;
        }
        double fsm_time_ms = fsm_timer.elapsed_ms();
        double fsm_avg_us = (fsm_time_ms * 1000.0) / ALLOC_QUERIES;

        std::cout << "  FSM Lookups:        " << ALLOC_QUERIES << "\n";
        std::cout << "  Total FSM time:     " << fsm_time_ms << " ms\n";
        std::cout << "  FSM Average Latency:" << fsm_avg_us << " us / allocation candidate\n";
    }

    std::filesystem::remove(db_path, ec);
    std::cout << "\n======================================================================\n";
    std::cout << "                 BENCHMARK EXECUTION COMPLETED                        \n";
    std::cout << "======================================================================\n";
    return 0;
}
