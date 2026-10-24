#include "test_framework.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/page.hpp"
#include <string>

using namespace klyrdb;
using namespace klyrdb::testing;

void test_fetch_page_from_disk() {
    auto path = temp_test_file("test_bpm_fetch");
    ScopedTempFile cleanup(path);

    // Create file and append a page with distinct data
    page_id_t data_pid = 0;
    {
        auto create_res = PageFile::create(path);
        EXPECT_TRUE(create_res.is_ok());
        auto& file = *create_res.value();

        Page p;
        p.set_page_type(PageType::Data);
        const std::string text = "Persistent disk content";
        EXPECT_TRUE(p.set_payload(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(text.data()), text.size())).is_ok());

        auto append_res = file.append_page(p);
        EXPECT_TRUE(append_res.is_ok());
        data_pid = append_res.value();
    }

    // Open file with BufferPoolManager
    auto open_res = PageFile::open(path);
    EXPECT_TRUE(open_res.is_ok());
    auto& file = *open_res.value();

    BufferPoolManager bpm(file, 10);
    EXPECT_EQ(bpm.pool_size(), 10U);
    EXPECT_EQ(bpm.free_frame_count(), 10U);
    EXPECT_FALSE(bpm.is_page_in_memory(data_pid));

    // Fetch page via BPM
    auto fetch_res = bpm.fetch_page(data_pid);
    EXPECT_TRUE(fetch_res.is_ok());
    Page* p = fetch_res.value();
    EXPECT_EQ(p->page_id(), data_pid);
    EXPECT_TRUE(bpm.is_page_in_memory(data_pid));
    EXPECT_EQ(bpm.pin_count(data_pid), 1U);
    EXPECT_EQ(bpm.free_frame_count(), 9U);

    auto payload = p->payload();
    std::string content(reinterpret_cast<const char*>(payload.data()), payload.size());
    EXPECT_EQ(content, "Persistent disk content");

    // Unpin
    EXPECT_TRUE(bpm.unpin_page(data_pid, false).is_ok());
    EXPECT_EQ(bpm.pin_count(data_pid), 0U);
}

void test_fetch_cached_page() {
    auto path = temp_test_file("test_bpm_cached");
    ScopedTempFile cleanup(path);

    page_id_t data_pid = 0;
    {
        auto create_res = PageFile::create(path);
        EXPECT_TRUE(create_res.is_ok());
        Page p;
        auto app_res = create_res.value()->append_page(p);
        EXPECT_TRUE(app_res.is_ok());
        data_pid = app_res.value();
    }

    auto open_res = PageFile::open(path);
    EXPECT_TRUE(open_res.is_ok());
    BufferPoolManager bpm(*open_res.value(), 10);

    // Fetch 1st time
    auto res1 = bpm.fetch_page(data_pid);
    EXPECT_TRUE(res1.is_ok());
    EXPECT_EQ(bpm.pin_count(data_pid), 1U);

    // Fetch 2nd time (should return cached frame without disk read)
    auto res2 = bpm.fetch_page(data_pid);
    EXPECT_TRUE(res2.is_ok());
    EXPECT_EQ(res1.value(), res2.value()); // Exact same memory address
    EXPECT_EQ(bpm.pin_count(data_pid), 2U);

    // Unpin both
    EXPECT_TRUE(bpm.unpin_page(data_pid, false).is_ok());
    EXPECT_EQ(bpm.pin_count(data_pid), 1U);
    EXPECT_TRUE(bpm.unpin_page(data_pid, false).is_ok());
    EXPECT_EQ(bpm.pin_count(data_pid), 0U);
}

void test_dirty_page_flushing() {
    auto path = temp_test_file("test_bpm_dirty_flush");
    ScopedTempFile cleanup(path);

    page_id_t data_pid = 0;
    {
        auto create_res = PageFile::create(path);
        EXPECT_TRUE(create_res.is_ok());
        Page p;
        auto app_res = create_res.value()->append_page(p);
        EXPECT_TRUE(app_res.is_ok());
        data_pid = app_res.value();
    }

    {
        auto open_res = PageFile::open(path);
        EXPECT_TRUE(open_res.is_ok());
        BufferPoolManager bpm(*open_res.value(), 5);

        auto fetch_res = bpm.fetch_page(data_pid);
        EXPECT_TRUE(fetch_res.is_ok());
        Page* p = fetch_res.value();

        const std::string new_content = "Modified in buffer pool";
        EXPECT_TRUE(p->set_payload(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(new_content.data()), new_content.size())).is_ok());

        EXPECT_TRUE(bpm.unpin_page(data_pid, true).is_ok());
        EXPECT_TRUE(bpm.is_dirty(data_pid));

        EXPECT_TRUE(bpm.flush_page(data_pid).is_ok());
        EXPECT_FALSE(bpm.is_dirty(data_pid));
    }

    // Reopen directly from disk and verify changes persisted
    {
        auto open_res = PageFile::open(path);
        EXPECT_TRUE(open_res.is_ok());
        Page p;
        EXPECT_TRUE(open_res.value()->read_page(data_pid, p).is_ok());
        auto payload = p.payload();
        std::string content(reinterpret_cast<const char*>(payload.data()), payload.size());
        EXPECT_EQ(content, "Modified in buffer pool");
    }
}

void test_eviction_under_pressure() {
    auto path = temp_test_file("test_bpm_eviction");
    ScopedTempFile cleanup(path);

    // Create 3 data pages (IDs 1, 2, 3)
    {
        auto create_res = PageFile::create(path);
        EXPECT_TRUE(create_res.is_ok());
        for (int i = 1; i <= 3; ++i) {
            Page p;
            p.set_page_type(PageType::Data);
            std::string text = "Page #" + std::to_string(i);
            EXPECT_TRUE(p.set_payload(std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(text.data()), text.size())).is_ok());
            EXPECT_TRUE(create_res.value()->append_page(p).is_ok());
        }
    }

    auto open_res = PageFile::open(path);
    EXPECT_TRUE(open_res.is_ok());

    // Buffer pool with ONLY 2 frames
    BufferPoolManager bpm(*open_res.value(), 2);
    EXPECT_EQ(bpm.pool_size(), 2U);

    // Load page 1 and unpin
    auto res1 = bpm.fetch_page(1);
    EXPECT_TRUE(res1.is_ok());
    EXPECT_TRUE(bpm.unpin_page(1, false).is_ok());

    // Load page 2 and unpin
    auto res2 = bpm.fetch_page(2);
    EXPECT_TRUE(res2.is_ok());
    EXPECT_TRUE(bpm.unpin_page(2, false).is_ok());

    EXPECT_TRUE(bpm.is_page_in_memory(1));
    EXPECT_TRUE(bpm.is_page_in_memory(2));
    EXPECT_EQ(bpm.free_frame_count(), 0U);

    // Load page 3: pool is full, so Clock algorithm must evict one unpinned page!
    auto res3 = bpm.fetch_page(3);
    EXPECT_TRUE(res3.is_ok());
    EXPECT_TRUE(bpm.is_page_in_memory(3));
    // At least one of page 1 or 2 must have been evicted
    EXPECT_TRUE(!bpm.is_page_in_memory(1) || !bpm.is_page_in_memory(2));
    EXPECT_TRUE(bpm.unpin_page(3, false).is_ok());

    // Re-fetch the evicted page and verify it reloads cleanly
    auto reload_res = bpm.fetch_page(1);
    EXPECT_TRUE(reload_res.is_ok());
    auto payload = reload_res.value()->payload();
    std::string text(reinterpret_cast<const char*>(payload.data()), payload.size());
    EXPECT_EQ(text, "Page #1");
    EXPECT_TRUE(bpm.unpin_page(1, false).is_ok());
}

void test_refusal_to_evict_pinned_pages() {
    auto path = temp_test_file("test_bpm_pinned");
    ScopedTempFile cleanup(path);

    {
        auto create_res = PageFile::create(path);
        EXPECT_TRUE(create_res.is_ok());
        for (int i = 1; i <= 3; ++i) {
            Page p;
            EXPECT_TRUE(create_res.value()->append_page(p).is_ok());
        }
    }

    auto open_res = PageFile::open(path);
    EXPECT_TRUE(open_res.is_ok());

    // Buffer pool of capacity 2
    BufferPoolManager bpm(*open_res.value(), 2);

    // Pin Page 1
    auto res1 = bpm.fetch_page(1);
    EXPECT_TRUE(res1.is_ok());

    // Pin Page 2
    auto res2 = bpm.fetch_page(2);
    EXPECT_TRUE(res2.is_ok());

    EXPECT_EQ(bpm.pinned_frame_count(), 2U);

    // Attempting to fetch Page 3 when ALL frames are pinned MUST fail with BufferPoolFull!
    auto res3 = bpm.fetch_page(3);
    EXPECT_FALSE(res3.is_ok());
    EXPECT_EQ(res3.status().code(), StatusCode::BufferPoolFull);

    // Unpin Page 1: now eviction is possible
    EXPECT_TRUE(bpm.unpin_page(1, false).is_ok());

    auto res3_retry = bpm.fetch_page(3);
    EXPECT_TRUE(res3_retry.is_ok());
    EXPECT_TRUE(bpm.is_page_in_memory(3));
    EXPECT_FALSE(bpm.is_page_in_memory(1)); // Page 1 was evicted

    EXPECT_TRUE(bpm.unpin_page(2, false).is_ok());
    EXPECT_TRUE(bpm.unpin_page(3, false).is_ok());
}

void test_page_guard_raii() {
    auto path = temp_test_file("test_bpm_guard");
    ScopedTempFile cleanup(path);

    page_id_t pid = 0;
    {
        auto create_res = PageFile::create(path);
        EXPECT_TRUE(create_res.is_ok());
        Page p;
        auto app_res = create_res.value()->append_page(p);
        EXPECT_TRUE(app_res.is_ok());
        pid = app_res.value();
    }

    auto open_res = PageFile::open(path);
    EXPECT_TRUE(open_res.is_ok());
    BufferPoolManager bpm(*open_res.value(), 5);

    {
        auto guard_res = bpm.fetch_page_guard(pid);
        EXPECT_TRUE(guard_res.is_ok());
        auto guard = std::move(guard_res.value());
        EXPECT_TRUE(guard.is_valid());
        EXPECT_EQ(guard.page_id(), pid);
        EXPECT_EQ(bpm.pin_count(pid), 1U);

        guard.mark_dirty();
        EXPECT_TRUE(guard.is_dirty());
    } // Guard destroyed here -> should unpin and mark dirty!

    EXPECT_EQ(bpm.pin_count(pid), 0U);
    EXPECT_TRUE(bpm.is_dirty(pid));
}

void test_invalid_page_id() {
    auto path = temp_test_file("test_bpm_invalid_id");
    ScopedTempFile cleanup(path);

    auto create_res = PageFile::create(path);
    EXPECT_TRUE(create_res.is_ok());
    BufferPoolManager bpm(*create_res.value(), 5);

    auto fetch_res = bpm.fetch_page(999);
    EXPECT_FALSE(fetch_res.is_ok());
    EXPECT_EQ(fetch_res.status().code(), StatusCode::InvalidPageId);
}

void test_delete_page() {
    auto path = temp_test_file("test_bpm_delete");
    ScopedTempFile cleanup(path);

    page_id_t pid = 0;
    {
        auto create_res = PageFile::create(path);
        EXPECT_TRUE(create_res.is_ok());
        Page p;
        auto app_res = create_res.value()->append_page(p);
        EXPECT_TRUE(app_res.is_ok());
        pid = app_res.value();
    }

    auto open_res = PageFile::open(path);
    EXPECT_TRUE(open_res.is_ok());
    BufferPoolManager bpm(*open_res.value(), 5);

    auto fetch_res = bpm.fetch_page(pid);
    EXPECT_TRUE(fetch_res.is_ok());

    // Cannot delete pinned page
    auto del_pinned = bpm.delete_page(pid);
    EXPECT_FALSE(del_pinned.is_ok());

    EXPECT_TRUE(bpm.unpin_page(pid, false).is_ok());
    EXPECT_TRUE(bpm.delete_page(pid).is_ok());
    EXPECT_FALSE(bpm.is_page_in_memory(pid));
    EXPECT_EQ(bpm.free_frame_count(), 5U);
}

int main() {
    std::cout << "Running BufferPoolManager Unit Tests...\n";
    run_test("Fetch Page From Disk", test_fetch_page_from_disk);
    run_test("Fetch Cached Page", test_fetch_cached_page);
    run_test("Dirty Page Flushing", test_dirty_page_flushing);
    run_test("Eviction Under Pressure", test_eviction_under_pressure);
    run_test("Refusal to Evict Pinned Pages", test_refusal_to_evict_pinned_pages);
    run_test("PageGuard RAII", test_page_guard_raii);
    run_test("Invalid Page ID", test_invalid_page_id);
    run_test("Delete Page", test_delete_page);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
