#include "test_framework.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/sql/executor.hpp"
#include "klyrdb/storage/integrity_checker.hpp"
#include <fstream>

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_detect_corrupted_page_checksum() {
    auto path = temp_test_file("test_corrupt_checksum");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");

    // 1. Create database and write a page
    {
        auto pf = PageFile::create(path, false).value();
        BufferPoolManager bpm(*pf, 16);
        page_id_t pid = 0;
        auto guard_res = bpm.new_page_guard(pid);
        EXPECT_TRUE(guard_res.is_ok());
        guard_res.value().mark_dirty();
        EXPECT_TRUE(bpm.flush_all_pages().is_ok());
        EXPECT_TRUE(pf->sync().is_ok());
    }

    // 2. Corrupt page 1 on disk by flipping a byte in its payload
    {
        std::fstream fs(path, std::ios::in | std::ios::out | std::ios::binary);
        EXPECT_TRUE(fs.is_open());
        // Page 1 starts at 4096 bytes
        fs.seekp(4096 + 30, std::ios::beg);
        uint8_t corrupt_byte = 0xAA;
        fs.write(reinterpret_cast<const char*>(&corrupt_byte), 1);
        fs.flush();
    }

    // 3. Run integrity checker
    auto pf = PageFile::open(path, false).value();
    IntegrityChecker checker(*pf, nullptr);
    auto rep_res = checker.check_all();
    EXPECT_TRUE(rep_res.is_ok());

    const auto& report = rep_res.value();
    EXPECT_FALSE(report.is_healthy);
    EXPECT_TRUE(report.pages_corrupted >= 1);

    bool found_checksum_error = false;
    for (const auto& issue : report.issues) {
        if (issue.subsystem == "Page" && issue.message.find("checksum mismatch") != std::string::npos) {
            found_checksum_error = true;
            break;
        }
    }
    EXPECT_TRUE(found_checksum_error);
}

static void test_detect_corrupted_slotted_page() {
    auto path = temp_test_file("test_corrupt_slotted");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");

    page_id_t record_pid = INVALID_PAGE_ID;
    {
        auto pf = PageFile::create(path, false).value();
        BufferPoolManager bpm(*pf, 16);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());

        std::vector<uint8_t> dummy_rec = {1, 2, 3, 4, 5};
        auto ins_res = rm.insert_record(dummy_rec);
        EXPECT_TRUE(ins_res.is_ok());
        record_pid = ins_res.value().page_id;

        EXPECT_TRUE(bpm.flush_all_pages().is_ok());
        EXPECT_TRUE(pf->sync().is_ok());
    }

    // Corrupt free space pointer in slotted page to an invalid huge value
    // while maintaining valid page CRC so check_slotted_pages detects the logical page corruption
    {
        auto pf = PageFile::open(path, false).value();
        Page page;
        EXPECT_TRUE(pf->read_page(record_pid, page).is_ok());
        auto p = page.mutable_payload();
        encode_u16_be(5000, p.data() + 2); // FSP = 5000 > MAX_PAYLOAD_SIZE (4072)
        EXPECT_TRUE(pf->write_page(page).is_ok());
        EXPECT_TRUE(pf->sync().is_ok());
    }

    auto pf = PageFile::open(path, false).value();
    IntegrityChecker checker(*pf, nullptr);
    auto rep_res = checker.check_all();
    EXPECT_TRUE(rep_res.is_ok());

    const auto& report = rep_res.value();
    EXPECT_FALSE(report.is_healthy);
    EXPECT_TRUE(report.error_count() >= 1);

    bool found_slotted_error = false;
    for (const auto& issue : report.issues) {
        if (issue.subsystem == "SlottedPage") {
            found_slotted_error = true;
            break;
        }
    }
    EXPECT_TRUE(found_slotted_error);
}

static void test_detect_corrupted_wal() {
    auto path = temp_test_file("test_corrupt_wal");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);

    auto pf = PageFile::create(path, false).value();

    // Create a 10-byte truncated WAL file
    {
        std::ofstream ws(wal_path, std::ios::binary | std::ios::trunc);
        char junk[10] = {0};
        ws.write(junk, 10);
    }

    IntegrityChecker checker(*pf, nullptr);
    auto rep_res = checker.check_all();
    EXPECT_TRUE(rep_res.is_ok());

    const auto& report = rep_res.value();
    EXPECT_FALSE(report.is_healthy);
    EXPECT_FALSE(report.wal_healthy);
}

int main() {
    std::cout << "=== Running Storage Corruption Detection Tests ===\n";
    run_test("test_detect_corrupted_page_checksum", test_detect_corrupted_page_checksum);
    run_test("test_detect_corrupted_slotted_page", test_detect_corrupted_slotted_page);
    run_test("test_detect_corrupted_wal", test_detect_corrupted_wal);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
