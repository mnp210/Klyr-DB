#include "test_framework.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/page_header.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/wal/log_record.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/recovery/recovery_manager.hpp"
#include "klyrdb/sql/executor.hpp"
#include "klyrdb/sql/value.hpp"
#include "klyrdb/common/endian.hpp"
#include <fstream>
#include <vector>
#include <cstring>

using namespace klyrdb;
using namespace klyrdb::testing;

static void test_wal_adversarial_malformed_headers() {
    // 1. Buffer smaller than LOG_RECORD_HEADER_SIZE (48 bytes)
    std::vector<uint8_t> small_buf(30, 0);
    auto res_small = LogRecord::decode(small_buf);
    EXPECT_TRUE(res_small.is_err());
    EXPECT_EQ(res_small.status().code(), StatusCode::TruncatedFile);

    // 2. Invalid magic
    std::vector<uint8_t> bad_magic_buf(LOG_RECORD_HEADER_SIZE, 0);
    encode_u32_be(0xDEADBEEF, bad_magic_buf.data());
    auto res_magic = LogRecord::decode(bad_magic_buf);
    EXPECT_TRUE(res_magic.is_err());
    EXPECT_EQ(res_magic.status().code(), StatusCode::InvalidMagic);

    // 3. Image length exceeds PAGE_SIZE (4096)
    LogRecord rec = LogRecord::make_begin(1, 10);
    std::vector<uint8_t> valid_buf;
    rec.encode(valid_buf);
    EXPECT_EQ(valid_buf.size(), LOG_RECORD_HEADER_SIZE);

    // Alter before_image_len to 5000 > PAGE_SIZE
    encode_u32_be(5000, valid_buf.data() + 36);
    auto res_len = LogRecord::decode(valid_buf);
    EXPECT_TRUE(res_len.is_err());
    EXPECT_EQ(res_len.status().code(), StatusCode::WalCorrupted);

    // 4. Corrupted CRC-32
    rec.encode(valid_buf);
    valid_buf[LOG_RECORD_HEADER_SIZE - 2] ^= 0x55; // flip byte in checksum
    auto res_crc = LogRecord::decode(valid_buf);
    EXPECT_TRUE(res_crc.is_err());
    EXPECT_EQ(res_crc.status().code(), StatusCode::ChecksumMismatch);
}

static void test_wal_file_header_adversarial() {
    // 1. Valid header encoding & decoding
    WalFileHeader hdr;
    hdr.checkpoint_lsn = 100;
    hdr.last_lsn = 200;
    std::array<uint8_t, WAL_HEADER_SIZE> buf;
    hdr.encode(buf);

    auto dec_res = WalFileHeader::decode(buf);
    EXPECT_TRUE(dec_res.is_ok());
    EXPECT_EQ(dec_res.value().checkpoint_lsn, 100ULL);
    EXPECT_EQ(dec_res.value().last_lsn, 200ULL);

    // 2. Corrupt magic
    encode_u32_be(0x12345678, buf.data());
    auto bad_magic = WalFileHeader::decode(buf);
    EXPECT_TRUE(bad_magic.is_err());
    EXPECT_EQ(bad_magic.status().code(), StatusCode::InvalidMagic);

    // 3. Corrupt page size != 4096
    hdr.encode(buf);
    encode_u16_be(8192, buf.data() + 6);
    auto bad_page = WalFileHeader::decode(buf);
    EXPECT_TRUE(bad_page.is_err());
    EXPECT_EQ(bad_page.status().code(), StatusCode::PageCorrupted);

    // 4. Corrupt checksum
    hdr.encode(buf);
    buf[WAL_HEADER_SIZE - 1] ^= 0xAA;
    auto bad_crc = WalFileHeader::decode(buf);
    EXPECT_TRUE(bad_crc.is_err());
    EXPECT_EQ(bad_crc.status().code(), StatusCode::ChecksumMismatch);
}

static void test_wal_truncated_mid_payload() {
    auto path = temp_test_file("test_wal_mid_payload");
    ScopedTempFile cleanup(path);
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);

    // Write a valid record first
    {
        auto wal_res = WriteAheadLog::open_or_create(wal_path);
        EXPECT_TRUE(wal_res.is_ok());
        auto& wal = *wal_res.value();
        LogRecord r1 = LogRecord::make_begin(INVALID_LSN, 1);
        EXPECT_TRUE(wal.append_record(r1).is_ok());
        EXPECT_TRUE(wal.flush().is_ok());
    }

    // Now append a record header specifying before_image_len = 200, but only write 50 bytes of payload
    {
        LogRecord partial;
        partial.type = LogRecordType::PageUpdate;
        partial.lsn = 2;
        partial.txn_id = 1;
        partial.page_id = 5;
        partial.before_image.resize(200, 0x55);
        std::vector<uint8_t> encoded;
        partial.encode(encoded);

        // Write only header + 50 bytes of the 200 byte payload
        std::ofstream out(wal_path, std::ios::binary | std::ios::app);
        out.write(reinterpret_cast<const char*>(encoded.data()), LOG_RECORD_HEADER_SIZE + 50);
        out.flush();
    }

    // Read back records: the truncated trailing write must be safely ignored as crash tail
    {
        auto wal_res = WriteAheadLog::open_or_create(wal_path);
        EXPECT_TRUE(wal_res.is_ok());
        auto recs_res = wal_res.value()->read_all_records();
        EXPECT_TRUE(recs_res.is_ok());
        EXPECT_EQ(recs_res.value().size(), 1ULL);
        EXPECT_EQ(recs_res.value()[0].type, LogRecordType::Begin);
    }
}

static void test_recovery_adversarial_idempotence() {
    auto path = temp_test_file("test_rec_idem");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);

    // 1. Create table, insert committed row, insert uncommitted row, crash
    {
        auto pf = PageFile::create(path, false).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        TransactionManager tm(bpm, *wal);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);
        executor.set_transaction_manager(&tm);

        EXPECT_TRUE(executor.execute("CREATE TABLE ledger (id INTEGER PRIMARY KEY, note TEXT);").is_ok());
        (void)bpm.flush_all_pages();

        // Committed transaction
        EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO ledger VALUES (1, 'Committed');").is_ok());
        EXPECT_TRUE(executor.execute("COMMIT;").is_ok());

        // Uncommitted transaction: insert row 2, dirty flushed to disk, then crash
        EXPECT_TRUE(executor.execute("BEGIN;").is_ok());
        EXPECT_TRUE(executor.execute("INSERT INTO ledger VALUES (2, 'Uncommitted');").is_ok());
        (void)bpm.flush_all_pages();
        bpm.drop_frames_without_flush();
    }

    // 2. First recovery: must redo row 1 and undo row 2
    {
        auto pf = PageFile::open(path, false).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        RecoveryManager rm(bpm, *wal);

        auto r1 = rm.recover();
        EXPECT_TRUE(r1.is_ok());
        EXPECT_EQ(r1.value().committed_txns, 1ULL);
        EXPECT_EQ(r1.value().uncommitted_txns, 1ULL);
    }

    // 3. Second recovery immediately: must be completely idempotent (0 redo, 0 undo)
    {
        auto pf = PageFile::open(path, false).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        RecoveryManager rm(bpm, *wal);

        auto r2 = rm.recover();
        EXPECT_TRUE(r2.is_ok());
        EXPECT_EQ(r2.value().pages_redone, 0ULL);
        EXPECT_EQ(r2.value().pages_undone, 0ULL);
        EXPECT_EQ(r2.value().uncommitted_txns, 0ULL);
    }

    // 4. Third recovery immediately: still idempotent
    {
        auto pf = PageFile::open(path, false).value();
        auto wal = WriteAheadLog::open_or_create(wal_path).value();
        BufferPoolManager bpm(*pf, 32);
        RecoveryManager rm(bpm, *wal);

        auto r3 = rm.recover();
        EXPECT_TRUE(r3.is_ok());
        EXPECT_EQ(r3.value().pages_redone, 0ULL);
        EXPECT_EQ(r3.value().pages_undone, 0ULL);
    }

    // 5. Verify final database state: row 1 exists, row 2 does not
    {
        auto pf = PageFile::open(path, false).value();
        BufferPoolManager bpm(*pf, 32);
        auto fsm = FreeSpaceMap::open_or_create(bpm).value();
        RecordManager rm(bpm, fsm.get());
        auto ic = IndexCatalog::open_or_create(bpm).value();
        auto tc = TableCatalog::open_or_create(bpm).value();
        SqlExecutor executor(bpm, rm, *tc, *ic);

        auto sel = executor.execute("SELECT * FROM ledger ORDER BY id ASC;");
        EXPECT_TRUE(sel.is_ok());
        EXPECT_EQ(sel.value().row_count(), 1ULL);
        EXPECT_EQ(sel.value().rows()[0].get(0).as_integer(), 1LL);
        EXPECT_EQ(sel.value().rows()[0].get(1).as_text(), "Committed");
    }
}

static void test_sql_adversarial_null_and_unique() {
    auto path = temp_test_file("test_sql_adv_null");
    ScopedTempFile cleanup(path);
    ScopedTempFile lock_cleanup(path.string() + ".lock");
    std::filesystem::path wal_path = path.string() + ".wal";
    ScopedTempFile wal_cleanup(wal_path);

    auto pf = PageFile::create(path, false).value();
    auto wal = WriteAheadLog::open_or_create(wal_path).value();
    BufferPoolManager bpm(*pf, 32);
    TransactionManager tm(bpm, *wal);
    auto fsm = FreeSpaceMap::open_or_create(bpm).value();
    RecordManager rm(bpm, fsm.get());
    auto ic = IndexCatalog::open_or_create(bpm).value();
    auto tc = TableCatalog::open_or_create(bpm).value();
    SqlExecutor executor(bpm, rm, *tc, *ic);
    executor.set_transaction_manager(&tm);

    // Create table with UNIQUE constraint on email
    EXPECT_TRUE(executor.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, email TEXT UNIQUE);").is_ok());

    // In SQL standard, multiple NULL values in UNIQUE columns are allowed
    EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (1, NULL);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (2, NULL);").is_ok());
    EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (3, NULL);").is_ok());

    auto sel_nulls = executor.execute("SELECT * FROM users;");
    EXPECT_TRUE(sel_nulls.is_ok());
    EXPECT_EQ(sel_nulls.value().row_count(), 3ULL);

    // Non-null unique value
    EXPECT_TRUE(executor.execute("INSERT INTO users VALUES (4, 'test@example.com');").is_ok());

    // Duplicate non-null value MUST fail
    auto dup_res = executor.execute("INSERT INTO users VALUES (5, 'test@example.com');");
    EXPECT_TRUE(dup_res.is_err());
    EXPECT_EQ(dup_res.status().code(), StatusCode::ConstraintViolation);

    // PRIMARY KEY column cannot be NULL
    auto pk_null_res = executor.execute("INSERT INTO users VALUES (NULL, 'other@example.com');");
    EXPECT_TRUE(pk_null_res.is_err());
    EXPECT_EQ(pk_null_res.status().code(), StatusCode::ConstraintViolation);
}

static void test_page_header_bounds_adversarial() {
    PageHeader hdr;
    std::array<uint8_t, PAGE_HEADER_SIZE> buf;

    // 1. Valid header
    hdr.encode(buf);
    auto res_ok = PageHeader::decode(buf);
    EXPECT_TRUE(res_ok.is_ok());

    // 2. Corrupted magic
    encode_u32_be(0xFEEDFACE, buf.data());
    auto res_magic = PageHeader::decode(buf);
    EXPECT_TRUE(res_magic.is_err());
    EXPECT_EQ(res_magic.status().code(), StatusCode::InvalidMagic);

    // 3. Payload size exceeds MAX_PAYLOAD_SIZE (4072)
    hdr.encode(buf);
    encode_u16_be(4073, buf.data() + OFFSET_PAGE_PAYLOAD_SIZE);
    auto res_size = PageHeader::decode(buf);
    EXPECT_TRUE(res_size.is_err());
    EXPECT_EQ(res_size.status().code(), StatusCode::PayloadOverflow);

    // 4. Invalid page type
    hdr.encode(buf);
    encode_u16_be(0x00FF, buf.data() + OFFSET_PAGE_TYPE);
    auto res_type = PageHeader::decode(buf);
    EXPECT_TRUE(res_type.is_err());
    EXPECT_EQ(res_type.status().code(), StatusCode::PageCorrupted);
}

int main() {
    std::cout << "=== KlyrDB Phase 13 Adversarial Audit Test Suite ===\n\n";

    run_test("test_wal_adversarial_malformed_headers", test_wal_adversarial_malformed_headers);
    run_test("test_wal_file_header_adversarial", test_wal_file_header_adversarial);
    run_test("test_wal_truncated_mid_payload", test_wal_truncated_mid_payload);
    run_test("test_recovery_adversarial_idempotence", test_recovery_adversarial_idempotence);
    run_test("test_sql_adversarial_null_and_unique", test_sql_adversarial_null_and_unique);
    run_test("test_page_header_bounds_adversarial", test_page_header_bounds_adversarial);

    std::cout << "\nTest Results: " << g_passed_tests << " passed, "
              << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
