#include "test_framework.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/page.hpp"
#include "klyrdb/storage/record.hpp"
#include <fstream>
#include <random>

using namespace klyrdb;
using namespace klyrdb::testing;

void test_full_roundtrip_persistence() {
    auto path = temp_test_file("test_roundtrip");
    ScopedTempFile cleanup(path);

    constexpr size_t NUM_PAGES = 10;
    std::vector<std::string> written_payloads;
    written_payloads.reserve(NUM_PAGES);

    // 1. Create file and append pages
    {
        auto create_res = PageFile::create(path);
        EXPECT_TRUE(create_res.is_ok());
        auto file = std::move(create_res.value());

        for (size_t i = 1; i <= NUM_PAGES; ++i) {
            std::string content = "Roundtrip Payload String for Page " + std::to_string(i) +
                                  " - Random Padding: " + std::string(i * 10, 'A' + static_cast<char>(i % 26));
            written_payloads.push_back(content);

            Page p;
            p.set_page_type(PageType::Data);
            EXPECT_TRUE(p.set_payload(std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(content.data()), content.size())).is_ok());

            auto append_res = file->append_page(p);
            EXPECT_TRUE(append_res.is_ok());
            EXPECT_EQ(append_res.value(), static_cast<page_id_t>(i));
        }

        EXPECT_EQ(file->page_count(), static_cast<uint32_t>(NUM_PAGES + 1));
        EXPECT_TRUE(file->sync().is_ok());
    } // File closed by RAII

    // 2. Reopen file in a completely fresh session
    {
        auto open_res = PageFile::open(path);
        EXPECT_TRUE(open_res.is_ok());
        auto file = std::move(open_res.value());

        EXPECT_EQ(file->page_count(), static_cast<uint32_t>(NUM_PAGES + 1));

        for (size_t i = 1; i <= NUM_PAGES; ++i) {
            Page p;
            auto read_res = file->read_page(static_cast<page_id_t>(i), p);
            EXPECT_TRUE(read_res.is_ok());
            EXPECT_EQ(p.page_id(), static_cast<page_id_t>(i));
            EXPECT_EQ(p.page_type(), PageType::Data);

            auto payload = p.payload();
            std::string recovered(reinterpret_cast<const char*>(payload.data()), payload.size());
            EXPECT_EQ(recovered, written_payloads[i - 1]);
        }
    }
}

void test_disk_corruption_detection() {
    auto path = temp_test_file("test_disk_corruption");
    ScopedTempFile cleanup(path);

    page_id_t target_pid = 0;
    // 1. Create file and append a page
    {
        auto create_res = PageFile::create(path);
        EXPECT_TRUE(create_res.is_ok());
        auto file = std::move(create_res.value());

        Page page;
        const std::string text = "Crucial Database Content that must not be corrupted";
        EXPECT_TRUE(page.set_payload(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(text.data()), text.size())).is_ok());

        auto append_res = file->append_page(page);
        EXPECT_TRUE(append_res.is_ok());
        target_pid = append_res.value();
    }

    // 2. Corrupt one byte on disk directly using raw file stream
    {
        std::fstream fs(path, std::ios::in | std::ios::out | std::ios::binary);
        EXPECT_TRUE(fs.is_open());
        // Seek to payload region of target page: pid * 4096 + 24 (payload offset) + 5
        auto corrupt_offset = static_cast<std::streamoff>(target_pid) * PAGE_SIZE + OFFSET_PAGE_PAYLOAD + 5;
        fs.seekp(corrupt_offset, std::ios::beg);
        char bad_byte = 0x7F;
        fs.write(&bad_byte, 1);
        fs.flush();
    }

    // 3. Reopen with KlyrDB and attempt to read the page
    {
        auto open_res = PageFile::open(path);
        EXPECT_TRUE(open_res.is_ok());
        auto file = std::move(open_res.value());

        Page page;
        auto read_status = file->read_page(target_pid, page);
        // Reading corrupted page must fail with ChecksumMismatch!
        EXPECT_FALSE(read_status.is_ok());
        EXPECT_EQ(read_status.code(), StatusCode::ChecksumMismatch);
    }
}

void test_record_persistence_in_page() {
    auto path = temp_test_file("test_record_persist");
    ScopedTempFile cleanup(path);

    Record original_record;
    original_record.add_int32(1001);
    original_record.add_string("Alice Smith");
    original_record.add_float64(98765.4321);
    original_record.add_null();
    original_record.add_int64(9223372036854775807LL);
    original_record.add_bytes({ 0x00, 0xFF, 0x42, 0xAA, 0x55 });

    std::vector<uint8_t> serialized_record = original_record.serialize();

    page_id_t record_pid = 0;
    // 1. Store record in a page and persist
    {
        auto create_res = PageFile::create(path);
        EXPECT_TRUE(create_res.is_ok());
        auto file = std::move(create_res.value());

        Page page;
        page.set_page_type(PageType::Record);
        EXPECT_TRUE(page.set_payload(serialized_record).is_ok());

        auto append_res = file->append_page(page);
        EXPECT_TRUE(append_res.is_ok());
        record_pid = append_res.value();
    }

    // 2. Reopen and deserialize record
    {
        auto open_res = PageFile::open(path);
        EXPECT_TRUE(open_res.is_ok());
        auto file = std::move(open_res.value());

        Page page;
        EXPECT_TRUE(file->read_page(record_pid, page).is_ok());
        EXPECT_EQ(page.page_type(), PageType::Record);

        auto dec_res = Record::deserialize(page.payload());
        EXPECT_TRUE(dec_res.is_ok());

        const Record& recovered = dec_res.value();
        EXPECT_EQ(recovered.field_count(), 6U);

        // Verify fields
        EXPECT_EQ(std::get<int32_t>(recovered.field(0)), 1001);
        EXPECT_EQ(std::get<std::string>(recovered.field(1)), "Alice Smith");
        EXPECT_EQ(std::get<double>(recovered.field(2)), 98765.4321);
        EXPECT_TRUE(std::holds_alternative<std::monostate>(recovered.field(3)));
        EXPECT_EQ(std::get<int64_t>(recovered.field(4)), 9223372036854775807LL);
        const auto& bytes = std::get<std::vector<uint8_t>>(recovered.field(5));
        EXPECT_EQ(bytes.size(), 5U);
        EXPECT_EQ(bytes[1], 0xFF);
        EXPECT_EQ(bytes[2], 0x42);
    }
}

int main() {
    std::cout << "Running Persistence Integration Tests...\n";
    run_test("Full Roundtrip Persistence", test_full_roundtrip_persistence);
    run_test("Disk Corruption Detection", test_disk_corruption_detection);
    run_test("Record Persistence in Page", test_record_persistence_in_page);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
