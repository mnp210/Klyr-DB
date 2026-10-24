#include "test_framework.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/record.hpp"
#include <string>
#include <vector>

using namespace klyrdb;
using namespace klyrdb::testing;

void test_insert_and_get_records() {
    auto path = temp_test_file("test_rm_insert_get");
    ScopedTempFile cleanup(path);

    auto create_res = PageFile::create(path);
    EXPECT_TRUE(create_res.is_ok());

    BufferPoolManager bpm(*create_res.value(), 10);
    RecordManager rm(bpm);

    std::vector<std::string> payloads = {
        "Record A",
        "Record B with a bit more length",
        "Record C"
    };

    std::vector<RecordId> rids;
    for (const auto& str : payloads) {
        auto ins = rm.insert_record(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(str.data()), str.size()));
        EXPECT_TRUE(ins.is_ok());
        EXPECT_TRUE(ins.value().is_valid());
        rids.push_back(ins.value());
    }

    // All should be on Page 1 (initial record page) with slots 0, 1, 2
    EXPECT_EQ(rids[0].page_id, 1U);
    EXPECT_EQ(rids[0].slot_id, 0U);
    EXPECT_EQ(rids[1].page_id, 1U);
    EXPECT_EQ(rids[1].slot_id, 1U);
    EXPECT_EQ(rids[2].page_id, 1U);
    EXPECT_EQ(rids[2].slot_id, 2U);

    // Retrieve and verify
    for (size_t i = 0; i < payloads.size(); ++i) {
        auto get_res = rm.get_record(rids[i]);
        EXPECT_TRUE(get_res.is_ok());
        std::string actual(reinterpret_cast<const char*>(get_res.value().data()), get_res.value().size());
        EXPECT_EQ(actual, payloads[i]);
    }
}

void test_records_distributed_across_pages() {
    auto path = temp_test_file("test_rm_multipage");
    ScopedTempFile cleanup(path);

    auto create_res = PageFile::create(path);
    EXPECT_TRUE(create_res.is_ok());

    BufferPoolManager bpm(*create_res.value(), 10);
    RecordManager rm(bpm);

    // Each record is 1000 bytes. With 4060 max capacity per page, a page fits 4 records.
    std::vector<RecordId> rids;
    constexpr size_t NUM_RECORDS = 10;
    std::vector<std::vector<uint8_t>> records_data;

    for (size_t i = 0; i < NUM_RECORDS; ++i) {
        std::vector<uint8_t> data(1000, static_cast<uint8_t>(i + 1));
        records_data.push_back(data);

        auto ins = rm.insert_record(data);
        EXPECT_TRUE(ins.is_ok());
        rids.push_back(ins.value());
    }

    // Verify that multiple pages were allocated
    EXPECT_TRUE(bpm.page_file().page_count() >= 3U);
    EXPECT_NE(rids.front().page_id, rids.back().page_id);

    // Retrieve and verify each record
    for (size_t i = 0; i < NUM_RECORDS; ++i) {
        auto get_res = rm.get_record(rids[i]);
        EXPECT_TRUE(get_res.is_ok());
        EXPECT_TRUE(get_res.value() == records_data[i]);
    }
}

void test_update_and_delete_record() {
    auto path = temp_test_file("test_rm_upd_del");
    ScopedTempFile cleanup(path);

    auto create_res = PageFile::create(path);
    EXPECT_TRUE(create_res.is_ok());

    BufferPoolManager bpm(*create_res.value(), 10);
    RecordManager rm(bpm);

    std::string original = "Original data";
    auto ins = rm.insert_record(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(original.data()), original.size()));
    EXPECT_TRUE(ins.is_ok());
    RecordId rid = ins.value();

    // Update
    std::string updated = "Updated data payload";
    EXPECT_TRUE(rm.update_record(rid, std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(updated.data()), updated.size())).is_ok());

    auto get_upd = rm.get_record(rid);
    EXPECT_TRUE(get_upd.is_ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(get_upd.value().data()), get_upd.value().size()), updated);

    // Delete
    EXPECT_TRUE(rm.delete_record(rid).is_ok());

    // Subsequent retrieval must fail with RecordNotFound
    auto get_del = rm.get_record(rid);
    EXPECT_FALSE(get_del.is_ok());
    EXPECT_EQ(get_del.status().code(), StatusCode::RecordNotFound);
}

void test_persistence_after_close_and_reopen() {
    auto path = temp_test_file("test_rm_persistence");
    ScopedTempFile cleanup(path);

    std::vector<RecordId> saved_rids;
    std::vector<std::string> saved_texts = {
        "Persistent record alpha",
        "Persistent record beta with extra text to test capacity",
        "Persistent record gamma",
        "Persistent record delta",
        "Persistent record epsilon"
    };

    // 1. Create, insert, flush and close
    {
        auto create_res = PageFile::create(path);
        EXPECT_TRUE(create_res.is_ok());

        BufferPoolManager bpm(*create_res.value(), 5);
        RecordManager rm(bpm);

        for (const auto& text : saved_texts) {
            auto ins = rm.insert_record(std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(text.data()), text.size()));
            EXPECT_TRUE(ins.is_ok());
            saved_rids.push_back(ins.value());
        }

        EXPECT_TRUE(bpm.flush_all_pages().is_ok());
    } // BPM and PageFile fully destroyed and closed

    // 2. Reopen fresh and retrieve
    {
        auto open_res = PageFile::open(path);
        EXPECT_TRUE(open_res.is_ok());

        BufferPoolManager bpm(*open_res.value(), 5);
        RecordManager rm(bpm);

        for (size_t i = 0; i < saved_texts.size(); ++i) {
            auto get_res = rm.get_record(saved_rids[i]);
            EXPECT_TRUE(get_res.is_ok());
            std::string actual(reinterpret_cast<const char*>(get_res.value().data()), get_res.value().size());
            EXPECT_EQ(actual, saved_texts[i]);
        }
    }
}

void test_typed_record_integration() {
    auto path = temp_test_file("test_rm_typed");
    ScopedTempFile cleanup(path);

    auto create_res = PageFile::create(path);
    EXPECT_TRUE(create_res.is_ok());

    BufferPoolManager bpm(*create_res.value(), 5);
    RecordManager rm(bpm);

    Record tuple;
    tuple.add_int32(42);
    tuple.add_string("Relational Database Row");
    tuple.add_float64(2.71828);
    tuple.add_null();
    tuple.add_int64(9876543210LL);

    auto ins_res = rm.insert_record(tuple);
    EXPECT_TRUE(ins_res.is_ok());
    RecordId rid = ins_res.value();

    auto get_res = rm.get_typed_record(rid);
    EXPECT_TRUE(get_res.is_ok());

    const Record& recovered = get_res.value();
    EXPECT_EQ(recovered.field_count(), 5U);
    EXPECT_EQ(std::get<int32_t>(recovered.field(0)), 42);
    EXPECT_EQ(std::get<std::string>(recovered.field(1)), "Relational Database Row");
    EXPECT_EQ(std::get<double>(recovered.field(2)), 2.71828);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(recovered.field(3)));
    EXPECT_EQ(std::get<int64_t>(recovered.field(4)), 9876543210LL);
}

void test_small_buffer_pool_thrashing() {
    auto path = temp_test_file("test_rm_thrash");
    ScopedTempFile cleanup(path);

    auto create_res = PageFile::create(path);
    EXPECT_TRUE(create_res.is_ok());

    // TINY buffer pool of capacity 2
    BufferPoolManager bpm(*create_res.value(), 2);
    RecordManager rm(bpm);

    // Insert 20 records of 800 bytes each -> spans ~5 distinct pages
    constexpr size_t COUNT = 20;
    std::vector<RecordId> rids;
    std::vector<std::vector<uint8_t>> expected_records;

    for (size_t i = 0; i < COUNT; ++i) {
        std::vector<uint8_t> data(800, static_cast<uint8_t>(i & 0xFF));
        expected_records.push_back(data);

        auto ins = rm.insert_record(data);
        EXPECT_TRUE(ins.is_ok());
        rids.push_back(ins.value());
    }

    // Now read them in reverse and random orders under severe eviction pressure
    for (int i = static_cast<int>(COUNT) - 1; i >= 0; --i) {
        auto get_res = rm.get_record(rids[i]);
        EXPECT_TRUE(get_res.is_ok());
        EXPECT_TRUE(get_res.value() == expected_records[i]);
    }
}

int main() {
    std::cout << "Running RecordManager Unit Tests...\n";
    run_test("Insert and Get Records", test_insert_and_get_records);
    run_test("Records Distributed Across Pages", test_records_distributed_across_pages);
    run_test("Update and Delete Record", test_update_and_delete_record);
    run_test("Persistence After Close and Reopen", test_persistence_after_close_and_reopen);
    run_test("Typed Record Integration", test_typed_record_integration);
    run_test("Small Buffer Pool Thrashing", test_small_buffer_pool_thrashing);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
