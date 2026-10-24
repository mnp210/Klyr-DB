#include "test_framework.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/page_guard.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <random>
#include <chrono>
#include <cstring>

using namespace klyrdb;
using namespace klyrdb::testing;
using namespace std::chrono_literals;

static void test_concurrent_fetch_and_unpin() {
    auto path = temp_test_file("test_bpm_concurrent_fetch");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");

    auto pf = PageFile::create(path, false).value();
    constexpr size_t pool_size = 8;
    constexpr size_t total_pages = 32;

    // Allocate 32 pages and populate each with a known byte pattern
    {
        BufferPoolManager bpm(*pf, pool_size);
        for (page_id_t p = 1; p <= total_pages; ++p) {
            page_id_t allocated_pid = 0;
            auto guard_res = bpm.new_page_guard(allocated_pid);
            EXPECT_TRUE(guard_res.is_ok());
            auto& guard = guard_res.value();
            EXPECT_EQ(allocated_pid, p);
            // Write page ID at offset 24 (after page header)
            uint32_t val = static_cast<uint32_t>(p * 100);
            std::memcpy(guard->raw_data_mut().data() + 24, &val, sizeof(val));
            guard.mark_dirty();
        }
        EXPECT_TRUE(bpm.flush_all_pages().is_ok());
    }

    // Now test concurrent readers with small pool size (8 frames, 32 pages => high eviction rate)
    BufferPoolManager bpm(*pf, pool_size);
    constexpr size_t num_threads = 8;
    constexpr size_t ops_per_thread = 100;
    std::atomic<size_t> success_count{0};
    std::atomic<size_t> verify_failures{0};
    std::vector<std::thread> workers;

    for (size_t t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, t]() {
            std::mt19937 rng(static_cast<unsigned int>(1337 + t));
            std::uniform_int_distribution<page_id_t> dist(1, total_pages);

            for (size_t i = 0; i < ops_per_thread; ++i) {
                page_id_t target = dist(rng);
                auto guard_res = bpm.fetch_page_guard(target);
                if (guard_res.is_ok()) {
                    auto& guard = guard_res.value();
                    uint32_t val = 0;
                    std::memcpy(&val, guard->raw_data().data() + 24, sizeof(val));
                    if (val != static_cast<uint32_t>(target * 100)) {
                        verify_failures.fetch_add(1);
                    }
                    success_count.fetch_add(1);
                    // guard auto-unpins when dropped
                } else {
                    verify_failures.fetch_add(1);
                }
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    EXPECT_EQ(verify_failures.load(), 0ULL);
    EXPECT_EQ(success_count.load(), num_threads * ops_per_thread);

    // Verify all frames are unpinned
    EXPECT_EQ(bpm.pinned_frame_count(), 0ULL);
}

static void test_concurrent_mutations_and_evictions() {
    auto path = temp_test_file("test_bpm_concurrent_mutations");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");

    auto pf = PageFile::create(path, false).value();
    constexpr size_t pool_size = 6;
    constexpr size_t total_pages = 24;

    {
        BufferPoolManager bpm(*pf, pool_size);
        for (page_id_t p = 1; p <= total_pages; ++p) {
            page_id_t allocated_pid = 0;
            auto guard_res = bpm.new_page_guard(allocated_pid);
            EXPECT_TRUE(guard_res.is_ok());
            auto& guard = guard_res.value();
            uint64_t initial_counter = 0;
            std::memcpy(guard->raw_data_mut().data() + 24, &initial_counter, sizeof(initial_counter));
            guard.mark_dirty();
        }
        EXPECT_TRUE(bpm.flush_all_pages().is_ok());
    }

    BufferPoolManager bpm(*pf, pool_size);
    constexpr size_t num_threads = 6;
    constexpr size_t iterations = 50;
    std::atomic<size_t> total_updates{0};
    std::vector<std::thread> workers;

    for (size_t t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, t]() {
            // Each thread owns a distinct subset of pages to avoid data races on the page memory
            page_id_t start_p = static_cast<page_id_t>(t * 4 + 1);
            for (size_t iter = 0; iter < iterations; ++iter) {
                for (page_id_t p = start_p; p < start_p + 4; ++p) {
                    auto guard_res = bpm.fetch_page_guard(p);
                    if (guard_res.is_ok()) {
                        auto& guard = guard_res.value();
                        uint64_t counter = 0;
                        std::memcpy(&counter, guard->raw_data().data() + 24, sizeof(counter));
                        counter += 1;
                        std::memcpy(guard->raw_data_mut().data() + 24, &counter, sizeof(counter));
                        guard.mark_dirty();
                        total_updates.fetch_add(1);
                    }
                }
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    EXPECT_EQ(total_updates.load(), num_threads * iterations * 4);
    EXPECT_TRUE(bpm.flush_all_pages().is_ok());
    EXPECT_EQ(bpm.pinned_frame_count(), 0ULL);

    // Verify all pages persisted their incremented counters
    for (page_id_t p = 1; p <= total_pages; ++p) {
        auto guard_res = bpm.fetch_page_guard(p);
        EXPECT_TRUE(guard_res.is_ok());
        uint64_t counter = 0;
        std::memcpy(&counter, guard_res.value()->raw_data().data() + 24, sizeof(counter));
        EXPECT_EQ(counter, static_cast<uint64_t>(iterations));
    }
}

int main() {
    std::cout << "=== Running BufferPool Concurrency Tests ===\n";
    run_test("test_concurrent_fetch_and_unpin", test_concurrent_fetch_and_unpin);
    run_test("test_concurrent_mutations_and_evictions", test_concurrent_mutations_and_evictions);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
