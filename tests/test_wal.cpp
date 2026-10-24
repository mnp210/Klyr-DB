#include "test_framework.hpp"
#include "klyrdb/wal/log_record.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include <fstream>

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_wal_record_encoding_decoding() {
    // 1. Begin Record
    {
        LogRecord rec = LogRecord::make_begin(1, 100);
        std::vector<uint8_t> buf;
        rec.encode(buf);
        EXPECT_EQ(buf.size(), LOG_RECORD_HEADER_SIZE);

        auto dec_res = LogRecord::decode(buf);
        EXPECT_TRUE(dec_res.is_ok());
        const auto& d = dec_res.value();
        EXPECT_EQ(d.type, LogRecordType::Begin);
        EXPECT_EQ(d.lsn, 1ULL);
        EXPECT_EQ(d.txn_id, 100ULL);
        EXPECT_EQ(d.prev_lsn, INVALID_LSN);
    }

    // 2. Page Update Record with full before and after images
    {
        std::vector<uint8_t> before_img(PAGE_SIZE, 0xAA);
        std::vector<uint8_t> after_img(PAGE_SIZE, 0xBB);

        LogRecord rec = LogRecord::make_page_update(2, 100, 1, 5, before_img, after_img);
        std::vector<uint8_t> buf;
        rec.encode(buf);
        EXPECT_EQ(buf.size(), LOG_RECORD_HEADER_SIZE + 2 * PAGE_SIZE);

        auto dec_res = LogRecord::decode(buf);
        EXPECT_TRUE(dec_res.is_ok());
        const auto& d = dec_res.value();
        EXPECT_EQ(d.type, LogRecordType::PageUpdate);
        EXPECT_EQ(d.lsn, 2ULL);
        EXPECT_EQ(d.txn_id, 100ULL);
        EXPECT_EQ(d.prev_lsn, 1ULL);
        EXPECT_EQ(d.page_id, 5U);
        EXPECT_EQ(d.before_image.size(), PAGE_SIZE);
        EXPECT_EQ(d.after_image.size(), PAGE_SIZE);
        EXPECT_EQ(d.before_image[0], 0xAA);
        EXPECT_EQ(d.after_image[0], 0xBB);
    }

    // 3. Commit Record
    {
        LogRecord rec = LogRecord::make_commit(3, 100, 2);
        std::vector<uint8_t> buf;
        rec.encode(buf);

        auto dec_res = LogRecord::decode(buf);
        EXPECT_TRUE(dec_res.is_ok());
        EXPECT_EQ(dec_res.value().type, LogRecordType::Commit);
        EXPECT_EQ(dec_res.value().lsn, 3ULL);
    }

    // 4. Abort Record
    {
        LogRecord rec = LogRecord::make_abort(4, 101, 1);
        std::vector<uint8_t> buf;
        rec.encode(buf);

        auto dec_res = LogRecord::decode(buf);
        EXPECT_TRUE(dec_res.is_ok());
        EXPECT_EQ(dec_res.value().type, LogRecordType::Abort);
    }

    // 5. Checkpoint Record
    {
        LogRecord rec = LogRecord::make_checkpoint(5, 4);
        std::vector<uint8_t> buf;
        rec.encode(buf);

        auto dec_res = LogRecord::decode(buf);
        EXPECT_TRUE(dec_res.is_ok());
        EXPECT_EQ(dec_res.value().type, LogRecordType::Checkpoint);
        EXPECT_EQ(dec_res.value().prev_lsn, 4ULL);
    }
}

static void test_wal_record_corruption_detection() {
    std::vector<uint8_t> before_img(PAGE_SIZE, 0x11);
    std::vector<uint8_t> after_img(PAGE_SIZE, 0x22);

    LogRecord rec = LogRecord::make_page_update(10, 50, INVALID_LSN, 1, before_img, after_img);
    std::vector<uint8_t> buf;
    rec.encode(buf);

    // Corrupt one payload byte
    buf[LOG_RECORD_HEADER_SIZE + 100] ^= 0xFF;
    auto dec_res = LogRecord::decode(buf);
    EXPECT_TRUE(dec_res.is_err());
    EXPECT_EQ(dec_res.status().code(), StatusCode::ChecksumMismatch);
}

static void test_wal_lifecycle_and_lsn_monotonicity() {
    auto path = temp_test_file("test_wal_lifecycle");
    ScopedTempFile cleanup(path);
    std::filesystem::path wal_path = path.string() + ".wal";

    {
        auto wal_res = WriteAheadLog::open_or_create(wal_path);
        EXPECT_TRUE(wal_res.is_ok());
        auto& wal = *wal_res.value();
        EXPECT_TRUE(wal.is_open());
        EXPECT_EQ(wal.last_lsn(), 0ULL);
        EXPECT_EQ(wal.flushed_lsn(), 0ULL);

        LogRecord r1 = LogRecord::make_begin(INVALID_LSN, 1);
        auto lsn1 = wal.append_record(r1);
        EXPECT_TRUE(lsn1.is_ok());
        EXPECT_EQ(lsn1.value(), 1ULL);
        EXPECT_EQ(wal.last_lsn(), 1ULL);

        LogRecord r2 = LogRecord::make_commit(INVALID_LSN, 1, 1);
        auto lsn2 = wal.append_record(r2);
        EXPECT_TRUE(lsn2.is_ok());
        EXPECT_EQ(lsn2.value(), 2ULL);
        EXPECT_EQ(wal.last_lsn(), 2ULL);

        // Durably flush
        EXPECT_TRUE(wal.flush().is_ok());
        EXPECT_EQ(wal.flushed_lsn(), 2ULL);
    }

    // Reopen and read back records
    {
        auto wal_res = WriteAheadLog::open_or_create(wal_path);
        EXPECT_TRUE(wal_res.is_ok());
        auto& wal = *wal_res.value();
        EXPECT_EQ(wal.last_lsn(), 2ULL);

        auto recs_res = wal.read_all_records();
        EXPECT_TRUE(recs_res.is_ok());
        const auto& recs = recs_res.value();
        EXPECT_EQ(recs.size(), 2ULL);
        EXPECT_EQ(recs[0].type, LogRecordType::Begin);
        EXPECT_EQ(recs[0].lsn, 1ULL);
        EXPECT_EQ(recs[1].type, LogRecordType::Commit);
        EXPECT_EQ(recs[1].lsn, 2ULL);
    }
}

static void test_wal_truncated_tail_handling() {
    auto path = temp_test_file("test_wal_tail");
    ScopedTempFile cleanup(path);
    std::filesystem::path wal_path = path.string() + ".wal";

    {
        auto wal_res = WriteAheadLog::open_or_create(wal_path);
        EXPECT_TRUE(wal_res.is_ok());
        auto& wal = *wal_res.value();

        LogRecord r1 = LogRecord::make_begin(INVALID_LSN, 1);
        (void)wal.append_record(r1);
        LogRecord r2 = LogRecord::make_commit(INVALID_LSN, 1, 1);
        (void)wal.append_record(r2);
        (void)wal.flush();
    }

    // Append partial trailing bytes (half of a record header) simulating a power cut mid-write
    {
        std::ofstream out(wal_path, std::ios::binary | std::ios::app);
        uint8_t partial_bytes[20] = {0x57, 0x41, 0x4C, 0x52, 0x04}; // magic + partial fields
        out.write(reinterpret_cast<const char*>(partial_bytes), sizeof(partial_bytes));
    }

    // Reopen WAL: read_all_records must detect EOF tail truncation cleanly
    {
        auto wal_res = WriteAheadLog::open_or_create(wal_path);
        EXPECT_TRUE(wal_res.is_ok());
        auto& wal = *wal_res.value();

        auto recs_res = wal.read_all_records();
        EXPECT_TRUE(recs_res.is_ok());
        EXPECT_EQ(recs_res.value().size(), 2ULL);
        EXPECT_EQ(recs_res.value()[0].type, LogRecordType::Begin);
        EXPECT_EQ(recs_res.value()[1].type, LogRecordType::Commit);
    }
}

static void test_wal_mid_log_corruption_rejection() {
    auto path = temp_test_file("test_wal_mid_corrupt");
    ScopedTempFile cleanup(path);
    std::filesystem::path wal_path = path.string() + ".wal";

    size_t corrupt_offset = 0;
    {
        auto wal_res = WriteAheadLog::open_or_create(wal_path);
        EXPECT_TRUE(wal_res.is_ok());
        auto& wal = *wal_res.value();

        LogRecord r1 = LogRecord::make_begin(INVALID_LSN, 1);
        (void)wal.append_record(r1);

        corrupt_offset = wal.file_size();

        LogRecord r2 = LogRecord::make_begin(INVALID_LSN, 2);
        (void)wal.append_record(r2);

        LogRecord r3 = LogRecord::make_commit(INVALID_LSN, 2, 2);
        (void)wal.append_record(r3);
        (void)wal.flush();
    }

    // Corrupt record 2 in the middle of the log file
    {
        std::fstream fs(wal_path, std::ios::in | std::ios::out | std::ios::binary);
        fs.seekp(static_cast<std::streamoff>(corrupt_offset + 10));
        char bad = '\xFF';
        fs.write(&bad, 1);
    }

    // Reopen WAL: must detect mid-log corruption and reject it
    {
        auto wal_res = WriteAheadLog::open_or_create(wal_path);
        EXPECT_TRUE(wal_res.is_ok());
        auto& wal = *wal_res.value();

        auto recs_res = wal.read_all_records();
        EXPECT_TRUE(recs_res.is_err());
        EXPECT_EQ(recs_res.status().code(), StatusCode::ChecksumMismatch);
    }
}

static void test_wal_checkpoint_and_truncation() {
    auto path = temp_test_file("test_wal_checkpoint");
    ScopedTempFile cleanup(path);
    std::filesystem::path wal_path = path.string() + ".wal";

    auto wal_res = WriteAheadLog::open_or_create(wal_path);
    EXPECT_TRUE(wal_res.is_ok());
    auto& wal = *wal_res.value();

    for (int i = 1; i <= 5; ++i) {
        LogRecord r = LogRecord::make_begin(INVALID_LSN, i);
        (void)wal.append_record(r);
    }
    (void)wal.flush();
    EXPECT_EQ(wal.last_lsn(), 5ULL);
    EXPECT_EQ(wal.file_size(), WAL_HEADER_SIZE + 5 * LOG_RECORD_HEADER_SIZE);

    // Truncate at LSN 5
    EXPECT_TRUE(wal.truncate(5).is_ok());
    EXPECT_EQ(wal.file_size(), WAL_HEADER_SIZE);
    EXPECT_EQ(wal.last_lsn(), 5ULL);

    // Append record after truncation
    LogRecord r6 = LogRecord::make_begin(INVALID_LSN, 6);
    auto lsn6 = wal.append_record(r6);
    EXPECT_TRUE(lsn6.is_ok());
    EXPECT_EQ(lsn6.value(), 6ULL);
}

int main() {
    std::cout << "=== KlyrDB Write-Ahead Log (WAL) Test Suite ===\n\n";

    run_test("test_wal_record_encoding_decoding", test_wal_record_encoding_decoding);
    run_test("test_wal_record_corruption_detection", test_wal_record_corruption_detection);
    run_test("test_wal_lifecycle_and_lsn_monotonicity", test_wal_lifecycle_and_lsn_monotonicity);
    run_test("test_wal_truncated_tail_handling", test_wal_truncated_tail_handling);
    run_test("test_wal_mid_log_corruption_rejection", test_wal_mid_log_corruption_rejection);
    run_test("test_wal_checkpoint_and_truncation", test_wal_checkpoint_and_truncation);

    std::cout << "\nTest Results: " << g_passed_tests << " passed, "
              << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
