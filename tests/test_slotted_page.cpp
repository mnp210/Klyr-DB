#include "test_framework.hpp"
#include "klyrdb/storage/slotted_page.hpp"
#include "klyrdb/storage/page.hpp"
#include <string>
#include <vector>
#include <numeric>
#include <cstring>

using namespace klyrdb;
using namespace klyrdb::testing;

void test_empty_page_initialization() {
    Page page;
    EXPECT_TRUE(SlottedPage::initialize(page).is_ok());
    EXPECT_EQ(page.page_type(), PageType::Record);
    EXPECT_TRUE(SlottedPage::is_slotted_page(page));

    SlottedPage slotted(page);
    EXPECT_EQ(slotted.slot_count(), 0U);
    EXPECT_EQ(slotted.deleted_slot_count(), 0U);
    EXPECT_EQ(slotted.contiguous_free_space(), static_cast<uint16_t>(MAX_PAYLOAD_SIZE - SLOTTED_PAGE_HEADER_SIZE));
    EXPECT_EQ(slotted.total_free_space(), static_cast<uint16_t>(MAX_PAYLOAD_SIZE - SLOTTED_PAGE_HEADER_SIZE));
}

void test_insert_and_retrieve_multiple_records() {
    Page page;
    EXPECT_TRUE(SlottedPage::initialize(page).is_ok());
    SlottedPage slotted(page);

    std::vector<std::string> test_strings;
    std::vector<slot_id_t> slot_ids;

    for (int i = 0; i < 10; ++i) {
        std::string s = "Slotted Record #" + std::to_string(i) + " [" + std::string(i * 15, 'x') + "]";
        test_strings.push_back(s);

        auto ins_res = slotted.insert_record(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(s.data()), s.size()));
        EXPECT_TRUE(ins_res.is_ok());
        slot_ids.push_back(ins_res.value());
        EXPECT_EQ(ins_res.value(), static_cast<slot_id_t>(i));
    }

    EXPECT_EQ(slotted.slot_count(), 10U);
    EXPECT_EQ(slotted.deleted_slot_count(), 0U);

    // Read back all 10 records and verify byte-for-byte fidelity
    for (size_t i = 0; i < 10; ++i) {
        auto rec_res = slotted.get_record(slot_ids[i]);
        EXPECT_TRUE(rec_res.is_ok());

        auto span = rec_res.value();
        std::string actual(reinterpret_cast<const char*>(span.data()), span.size());
        EXPECT_EQ(actual, test_strings[i]);
    }
}

void test_empty_record() {
    Page page;
    EXPECT_TRUE(SlottedPage::initialize(page).is_ok());
    SlottedPage slotted(page);

    std::span<const uint8_t> empty_data{};
    auto ins_res = slotted.insert_record(empty_data);
    EXPECT_TRUE(ins_res.is_ok());
    EXPECT_EQ(ins_res.value(), 0U);

    auto rec_res = slotted.get_record(0);
    EXPECT_TRUE(rec_res.is_ok());
    EXPECT_EQ(rec_res.value().size(), 0U);
}

void test_maximum_size_record() {
    Page page;
    EXPECT_TRUE(SlottedPage::initialize(page).is_ok());
    SlottedPage slotted(page);

    std::vector<uint8_t> max_rec(MAX_SLOTTED_RECORD_SIZE);
    std::iota(max_rec.begin(), max_rec.end(), static_cast<uint8_t>(1));

    auto ins_res = slotted.insert_record(max_rec);
    EXPECT_TRUE(ins_res.is_ok());
    EXPECT_EQ(ins_res.value(), 0U);
    EXPECT_EQ(slotted.contiguous_free_space(), 0U);
    EXPECT_EQ(slotted.total_free_space(), 0U);

    auto rec_res = slotted.get_record(0);
    EXPECT_TRUE(rec_res.is_ok());
    EXPECT_EQ(rec_res.value().size(), MAX_SLOTTED_RECORD_SIZE);
    EXPECT_EQ(std::memcmp(rec_res.value().data(), max_rec.data(), MAX_SLOTTED_RECORD_SIZE), 0);
}

void test_page_capacity_boundary() {
    Page page;
    EXPECT_TRUE(SlottedPage::initialize(page).is_ok());
    SlottedPage slotted(page);

    // Record exceeding MAX_SLOTTED_RECORD_SIZE
    std::vector<uint8_t> huge_rec(MAX_SLOTTED_RECORD_SIZE + 1, 0xFF);
    auto ins_res = slotted.insert_record(huge_rec);
    EXPECT_FALSE(ins_res.is_ok());
    EXPECT_EQ(ins_res.status().code(), StatusCode::PayloadOverflow);

    // Insert max size record
    std::vector<uint8_t> max_rec(MAX_SLOTTED_RECORD_SIZE, 0x42);
    EXPECT_TRUE(slotted.insert_record(max_rec).is_ok());

    // Subsequent insertion must fail with NoFreeSpace
    std::vector<uint8_t> extra_rec(1, 0x99);
    auto full_res = slotted.insert_record(extra_rec);
    EXPECT_FALSE(full_res.is_ok());
    EXPECT_EQ(full_res.status().code(), StatusCode::NoFreeSpace);
}

void test_record_deletion_tombstone() {
    Page page;
    EXPECT_TRUE(SlottedPage::initialize(page).is_ok());
    SlottedPage slotted(page);

    std::string s0 = "Record Zero";
    std::string s1 = "Record One (to be deleted)";
    std::string s2 = "Record Two";

    auto r0 = slotted.insert_record(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s0.data()), s0.size()));
    auto r1 = slotted.insert_record(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s1.data()), s1.size()));
    auto r2 = slotted.insert_record(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s2.data()), s2.size()));
    EXPECT_TRUE(r0.is_ok() && r1.is_ok() && r2.is_ok());

    // Delete slot 1
    EXPECT_TRUE(slotted.delete_record(r1.value()).is_ok());
    EXPECT_EQ(slotted.deleted_slot_count(), 1U);

    // Getting slot 1 must fail with RecordNotFound
    auto get1 = slotted.get_record(r1.value());
    EXPECT_FALSE(get1.is_ok());
    EXPECT_EQ(get1.status().code(), StatusCode::RecordNotFound);

    // Slots 0 and 2 must remain valid and intact
    auto get0 = slotted.get_record(r0.value());
    EXPECT_TRUE(get0.is_ok());
    std::string act0(reinterpret_cast<const char*>(get0.value().data()), get0.value().size());
    EXPECT_EQ(act0, s0);

    auto get2 = slotted.get_record(r2.value());
    EXPECT_TRUE(get2.is_ok());
    std::string act2(reinterpret_cast<const char*>(get2.value().data()), get2.value().size());
    EXPECT_EQ(act2, s2);
}

void test_slot_reuse_after_deletion() {
    Page page;
    EXPECT_TRUE(SlottedPage::initialize(page).is_ok());
    SlottedPage slotted(page);

    std::string r0_data = "Item 0";
    std::string r1_data = "Item 1 (temp)";
    std::string r2_data = "Item 2";

    auto id0 = slotted.insert_record(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(r0_data.data()), r0_data.size())).value();
    auto id1 = slotted.insert_record(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(r1_data.data()), r1_data.size())).value();
    auto id2 = slotted.insert_record(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(r2_data.data()), r2_data.size())).value();
    EXPECT_EQ(id0, 0U);
    EXPECT_EQ(id1, 1U);
    EXPECT_EQ(id2, 2U);

    // Delete slot 1
    EXPECT_TRUE(slotted.delete_record(1).is_ok());
    EXPECT_EQ(slotted.deleted_slot_count(), 1U);

    // Insert a new record: it MUST reuse slot ID 1
    std::string replacement = "Replaced Item 1";
    auto new_id = slotted.insert_record(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(replacement.data()), replacement.size()));
    EXPECT_TRUE(new_id.is_ok());
    EXPECT_EQ(new_id.value(), 1U); // Reused slot 1
    EXPECT_EQ(slotted.deleted_slot_count(), 0U);
    EXPECT_EQ(slotted.slot_count(), 3U); // Did not grow slot directory

    auto rec = slotted.get_record(1);
    EXPECT_TRUE(rec.is_ok());
    std::string actual(reinterpret_cast<const char*>(rec.value().data()), rec.value().size());
    EXPECT_EQ(actual, replacement);
}

void test_record_update_shrink_and_grow() {
    Page page;
    EXPECT_TRUE(SlottedPage::initialize(page).is_ok());
    SlottedPage slotted(page);

    std::string orig = "Original Record With Decent Length";
    auto ins = slotted.insert_record(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(orig.data()), orig.size()));
    EXPECT_TRUE(ins.is_ok());
    const slot_id_t sid = ins.value();

    // 1. Shrink update
    std::string shrunk = "Short";
    EXPECT_TRUE(slotted.update_record(sid, std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(shrunk.data()), shrunk.size())).is_ok());

    auto get_shrunk = slotted.get_record(sid);
    EXPECT_TRUE(get_shrunk.is_ok());
    std::string act_shrunk(reinterpret_cast<const char*>(get_shrunk.value().data()), get_shrunk.value().size());
    EXPECT_EQ(act_shrunk, shrunk);

    // 2. Grow update
    std::string grown = "Much Larger Record That Expands Beyond Previous Length And Checks Defrag";
    EXPECT_TRUE(slotted.update_record(sid, std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(grown.data()), grown.size())).is_ok());

    auto get_grown = slotted.get_record(sid);
    EXPECT_TRUE(get_grown.is_ok());
    std::string act_grown(reinterpret_cast<const char*>(get_grown.value().data()), get_grown.value().size());
    EXPECT_EQ(act_grown, grown);
}

void test_compaction_and_stable_slot_ids() {
    Page page;
    EXPECT_TRUE(SlottedPage::initialize(page).is_ok());
    SlottedPage slotted(page);

    std::vector<std::string> items = {
        "First item at slot 0",
        "Second item to delete at slot 1",
        "Third item at slot 2",
        "Fourth item to delete at slot 3",
        "Fifth item at slot 4"
    };

    for (size_t i = 0; i < items.size(); ++i) {
        auto ins = slotted.insert_record(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(items[i].data()), items[i].size()));
        EXPECT_TRUE(ins.is_ok());
        EXPECT_EQ(ins.value(), static_cast<slot_id_t>(i));
    }

    // Delete slots 1 and 3
    EXPECT_TRUE(slotted.delete_record(1).is_ok());
    EXPECT_TRUE(slotted.delete_record(3).is_ok());

    // Explicitly compact
    EXPECT_TRUE(slotted.compact().is_ok());

    // Crucial Invariant: Slots 0, 2, and 4 must retain their exact slot IDs and exact contents!
    EXPECT_EQ(slotted.slot_count(), 5U);
    EXPECT_EQ(slotted.deleted_slot_count(), 2U);

    auto r0 = slotted.get_record(0);
    EXPECT_TRUE(r0.is_ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(r0.value().data()), r0.value().size()), items[0]);

    auto r2 = slotted.get_record(2);
    EXPECT_TRUE(r2.is_ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(r2.value().data()), r2.value().size()), items[2]);

    auto r4 = slotted.get_record(4);
    EXPECT_TRUE(r4.is_ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(r4.value().data()), r4.value().size()), items[4]);

    // Slots 1 and 3 are still deleted
    EXPECT_FALSE(slotted.get_record(1).is_ok());
    EXPECT_FALSE(slotted.get_record(3).is_ok());
}

void test_invalid_slot_id_and_corruption() {
    Page page;
    EXPECT_TRUE(SlottedPage::initialize(page).is_ok());
    SlottedPage slotted(page);

    auto bad_get = slotted.get_record(99);
    EXPECT_FALSE(bad_get.is_ok());
    EXPECT_EQ(bad_get.status().code(), StatusCode::InvalidSlotId);

    auto bad_del = slotted.delete_record(99);
    EXPECT_FALSE(bad_del.is_ok());
    EXPECT_EQ(bad_del.code(), StatusCode::InvalidSlotId);

    std::vector<uint8_t> dummy(10, 0x01);
    auto bad_upd = slotted.update_record(99, dummy);
    EXPECT_FALSE(bad_upd.is_ok());
    EXPECT_EQ(bad_upd.code(), StatusCode::InvalidSlotId);
}

int main() {
    std::cout << "Running SlottedPage Unit Tests...\n";
    run_test("Empty Page Initialization", test_empty_page_initialization);
    run_test("Insert and Retrieve Multiple Records", test_insert_and_retrieve_multiple_records);
    run_test("Empty Record", test_empty_record);
    run_test("Maximum Size Record", test_maximum_size_record);
    run_test("Page Capacity Boundary", test_page_capacity_boundary);
    run_test("Record Deletion Tombstone", test_record_deletion_tombstone);
    run_test("Slot Reuse After Deletion", test_slot_reuse_after_deletion);
    run_test("Record Update Shrink and Grow", test_record_update_shrink_and_grow);
    run_test("Compaction and Stable Slot IDs", test_compaction_and_stable_slot_ids);
    run_test("Invalid Slot ID", test_invalid_slot_id_and_corruption);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
