#include "test_framework.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/page.hpp"
#include <fstream>

using namespace klyrdb;
using namespace klyrdb::testing;

void test_file_create_and_reopen() {
    auto path = temp_test_file("test_create");
    ScopedTempFile cleanup(path);

    // 1. Create file
    auto create_res = PageFile::create(path);
    EXPECT_TRUE(create_res.is_ok());
    {
        auto file = std::move(create_res.value());
        EXPECT_EQ(file->page_count(), 1U); // Page 0 initialized
        EXPECT_TRUE(file->is_open());
    }

    // 2. Open existing file
    auto open_res = PageFile::open(path);
    EXPECT_TRUE(open_res.is_ok());
    {
        auto file = std::move(open_res.value());
        EXPECT_EQ(file->page_count(), 1U);
        EXPECT_EQ(file->metadata().db_magic, DB_MAGIC);
        EXPECT_EQ(file->metadata().db_version, DB_FORMAT_VERSION);
    }
}

void test_reject_existing_file_on_create() {
    auto path = temp_test_file("test_no_overwrite");
    ScopedTempFile cleanup(path);

    auto create_res = PageFile::create(path);
    EXPECT_TRUE(create_res.is_ok());

    // Creating again on same path must fail with FileAlreadyExists
    auto second_create = PageFile::create(path);
    EXPECT_FALSE(second_create.is_ok());
    EXPECT_EQ(second_create.status().code(), StatusCode::FileAlreadyExists);
}

void test_open_nonexistent_file() {
    auto path = temp_test_file("nonexistent_db");
    // Ensure file definitely doesn't exist
    std::filesystem::remove(path);

    auto open_res = PageFile::open(path);
    EXPECT_FALSE(open_res.is_ok());
    EXPECT_EQ(open_res.status().code(), StatusCode::FileNotFound);
}

void test_multiple_page_appends_and_reads() {
    auto path = temp_test_file("test_appends");
    ScopedTempFile cleanup(path);

    auto create_res = PageFile::create(path);
    EXPECT_TRUE(create_res.is_ok());
    auto file = std::move(create_res.value());

    // Append 5 pages with distinct content
    for (uint32_t i = 1; i <= 5; ++i) {
        Page page;
        page.set_page_type(PageType::Data);
        std::string payload = "Page Payload #" + std::to_string(i);
        EXPECT_TRUE(page.set_payload(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(payload.data()), payload.size())).is_ok());

        auto append_res = file->append_page(page);
        EXPECT_TRUE(append_res.is_ok());
        EXPECT_EQ(append_res.value(), i);
    }

    EXPECT_EQ(file->page_count(), 6U); // Page 0 + 5 appended pages

    // Read back each page and verify
    for (uint32_t i = 1; i <= 5; ++i) {
        Page read_page;
        auto s = file->read_page(i, read_page);
        EXPECT_TRUE(s.is_ok());
        EXPECT_EQ(read_page.page_id(), i);
        EXPECT_EQ(read_page.page_type(), PageType::Data);

        std::string expected_payload = "Page Payload #" + std::to_string(i);
        auto p = read_page.payload();
        std::string actual_payload(reinterpret_cast<const char*>(p.data()), p.size());
        EXPECT_EQ(actual_payload, expected_payload);
    }

    // Verify out-of-range read
    Page out_of_bounds;
    auto oob_status = file->read_page(99, out_of_bounds);
    EXPECT_FALSE(oob_status.is_ok());
    EXPECT_EQ(oob_status.code(), StatusCode::InvalidPageId);
}

void test_detect_truncated_files() {
    auto path = temp_test_file("test_truncated");
    ScopedTempFile cleanup(path);

    // 1. Create a 0-byte file
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    }
    auto open_empty = PageFile::open(path);
    EXPECT_FALSE(open_empty.is_ok());
    EXPECT_EQ(open_empty.status().code(), StatusCode::TruncatedFile);

    // 2. Create a file whose size is not a multiple of 4096 (e.g. 500 bytes)
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        std::vector<uint8_t> dummy(500, 0xAA);
        ofs.write(reinterpret_cast<const char*>(dummy.data()), dummy.size());
    }
    auto open_non_mult = PageFile::open(path);
    EXPECT_FALSE(open_non_mult.is_ok());
    EXPECT_EQ(open_non_mult.status().code(), StatusCode::TruncatedFile);
}

void test_update_existing_page() {
    auto path = temp_test_file("test_update");
    ScopedTempFile cleanup(path);

    auto create_res = PageFile::create(path);
    EXPECT_TRUE(create_res.is_ok());
    auto file = std::move(create_res.value());

    Page page;
    const std::string initial_text = "Initial Content";
    EXPECT_TRUE(page.set_payload(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(initial_text.data()), initial_text.size())).is_ok());
    auto app_res = file->append_page(page);
    EXPECT_TRUE(app_res.is_ok());
    const page_id_t pid = app_res.value();

    // Overwrite the existing page with updated content
    Page updated_page(pid, PageType::Data);
    const std::string updated_text = "Updated Content In Place";
    EXPECT_TRUE(updated_page.set_payload(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(updated_text.data()), updated_text.size())).is_ok());
    EXPECT_TRUE(file->write_page(updated_page).is_ok());

    // Read back and confirm update
    Page verify_page;
    EXPECT_TRUE(file->read_page(pid, verify_page).is_ok());
    auto p = verify_page.payload();
    std::string actual(reinterpret_cast<const char*>(p.data()), p.size());
    EXPECT_EQ(actual, updated_text);
}

int main() {
    std::cout << "Running PageFile Unit Tests...\n";
    run_test("File Create and Reopen", test_file_create_and_reopen);
    run_test("Reject Existing File on Create", test_reject_existing_file_on_create);
    run_test("Open Nonexistent File", test_open_nonexistent_file);
    run_test("Multiple Page Appends and Reads", test_multiple_page_appends_and_reads);
    run_test("Detect Truncated Files", test_detect_truncated_files);
    run_test("Update Existing Page", test_update_existing_page);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
