#include "test_framework.hpp"
#include "klyrdb/storage/page.hpp"
#include "klyrdb/storage/page_header.hpp"
#include "klyrdb/common/endian.hpp"
#include "klyrdb/common/checksum.hpp"
#include <vector>
#include <numeric>

using namespace klyrdb;
using namespace klyrdb::testing;

void test_endian_roundtrip() {
    uint8_t buf[16]{};
    encode_u16_be(0x1234, buf);
    EXPECT_EQ(buf[0], 0x12);
    EXPECT_EQ(buf[1], 0x34);
    EXPECT_EQ(decode_u16_be(buf), 0x1234);

    encode_u32_be(0xDEADBEEF, buf);
    EXPECT_EQ(buf[0], 0xDE);
    EXPECT_EQ(buf[1], 0xAD);
    EXPECT_EQ(buf[2], 0xBE);
    EXPECT_EQ(buf[3], 0xEF);
    EXPECT_EQ(decode_u32_be(buf), 0xDEADBEEF);

    encode_u64_be(0x0123456789ABCDEFULL, buf);
    EXPECT_EQ(decode_u64_be(buf), 0x0123456789ABCDEFULL);

    encode_f64_be(3.141592653589793, buf);
    EXPECT_EQ(decode_f64_be(buf), 3.141592653589793);
}

void test_checksum_properties() {
    const std::vector<uint8_t> data1 = { 'H', 'e', 'l', 'l', 'o' };
    const uint32_t crc1 = calculate_crc32(data1);
    EXPECT_NE(crc1, 0U);

    // Consistency check
    EXPECT_EQ(calculate_crc32(data1), crc1);

    // Bit flip detection
    std::vector<uint8_t> data2 = data1;
    data2[0] ^= 0x01; // single bit flip
    const uint32_t crc2 = calculate_crc32(data2);
    EXPECT_NE(crc1, crc2);
}

void test_page_initialization() {
    Page page(42, PageType::Data);
    EXPECT_EQ(page.page_id(), 42U);
    EXPECT_EQ(page.page_type(), PageType::Data);
    EXPECT_EQ(page.payload_size(), 0U);
    EXPECT_EQ(page.payload().size(), 0U);
    EXPECT_EQ(PAGE_SIZE, 4096U);
    EXPECT_EQ(PAGE_HEADER_SIZE, 24U);
    EXPECT_EQ(MAX_PAYLOAD_SIZE, 4072U);
}

void test_page_serialization_deserialization() {
    Page page(10, PageType::Data);
    const std::string text = "Hello, KlyrDB Page Storage!";
    auto s = page.set_payload(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(text.data()), text.size()));
    EXPECT_TRUE(s.is_ok());
    EXPECT_EQ(page.payload_size(), static_cast<uint16_t>(text.size()));

    std::array<uint8_t, PAGE_SIZE> buffer{};
    s = page.serialize(buffer);
    EXPECT_TRUE(s.is_ok());

    // Verify exact serialized size
    EXPECT_EQ(buffer.size(), 4096U);

    // Deserialize into fresh page
    Page restored_page;
    s = restored_page.deserialize(buffer);
    EXPECT_TRUE(s.is_ok());
    EXPECT_EQ(restored_page.page_id(), 10U);
    EXPECT_EQ(restored_page.page_type(), PageType::Data);
    EXPECT_EQ(restored_page.payload_size(), static_cast<uint16_t>(text.size()));

    auto restored_span = restored_page.payload();
    std::string restored_text(reinterpret_cast<const char*>(restored_span.data()), restored_span.size());
    EXPECT_EQ(restored_text, text);
}

void test_empty_payload() {
    Page page(1, PageType::Data);
    EXPECT_EQ(page.payload_size(), 0U);

    std::array<uint8_t, PAGE_SIZE> buffer{};
    Status s = page.serialize(buffer);
    EXPECT_TRUE(s.is_ok());

    Page restored;
    s = restored.deserialize(buffer);
    EXPECT_TRUE(s.is_ok());
    EXPECT_EQ(restored.payload_size(), 0U);
    EXPECT_TRUE(restored.payload().empty());
}

void test_maximum_payload() {
    Page page(2, PageType::Data);
    std::vector<uint8_t> max_payload(MAX_PAYLOAD_SIZE);
    std::iota(max_payload.begin(), max_payload.end(), static_cast<uint8_t>(0));

    Status s = page.set_payload(max_payload);
    EXPECT_TRUE(s.is_ok());
    EXPECT_EQ(page.payload_size(), static_cast<uint16_t>(MAX_PAYLOAD_SIZE));

    std::array<uint8_t, PAGE_SIZE> buffer{};
    s = page.serialize(buffer);
    EXPECT_TRUE(s.is_ok());

    Page restored;
    s = restored.deserialize(buffer);
    EXPECT_TRUE(s.is_ok());
    EXPECT_EQ(restored.payload_size(), static_cast<uint16_t>(MAX_PAYLOAD_SIZE));
    EXPECT_EQ(std::memcmp(restored.payload().data(), max_payload.data(), MAX_PAYLOAD_SIZE), 0);
}

void test_payload_overflow_rejection() {
    Page page(3, PageType::Data);
    std::vector<uint8_t> oversized_payload(MAX_PAYLOAD_SIZE + 1, 0xFF);

    Status s = page.set_payload(oversized_payload);
    EXPECT_FALSE(s.is_ok());
    EXPECT_EQ(s.code(), StatusCode::PayloadOverflow);

    // Test offset overflow
    std::vector<uint8_t> chunk(10, 0xAA);
    s = page.write_payload(MAX_PAYLOAD_SIZE - 5, chunk);
    EXPECT_FALSE(s.is_ok());
    EXPECT_EQ(s.code(), StatusCode::PayloadOverflow);
}

void test_corrupted_checksum_rejection() {
    Page page(4, PageType::Data);
    const std::string text = "Integrity check test string";
    (void)page.set_payload(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(text.data()), text.size()));

    std::array<uint8_t, PAGE_SIZE> buffer{};
    EXPECT_TRUE(page.serialize(buffer).is_ok());

    // Corrupt one payload byte in the serialized stream
    buffer[OFFSET_PAGE_PAYLOAD + 2] ^= 0x55;

    Page bad_page;
    Status s = bad_page.deserialize(buffer);
    EXPECT_FALSE(s.is_ok());
    EXPECT_EQ(s.code(), StatusCode::ChecksumMismatch);
}

void test_incorrect_magic_rejection() {
    Page page(5, PageType::Data);
    std::array<uint8_t, PAGE_SIZE> buffer{};
    EXPECT_TRUE(page.serialize(buffer).is_ok());

    // Tamper with magic signature bytes
    encode_u32_be(0xBAD0C0DE, buffer.data() + OFFSET_PAGE_MAGIC);

    // Recompute valid checksum for this tampered buffer to isolate magic check
    encode_u32_be(0, buffer.data() + OFFSET_PAGE_CHECKSUM);
    uint32_t new_crc = calculate_crc32(buffer);
    encode_u32_be(new_crc, buffer.data() + OFFSET_PAGE_CHECKSUM);

    Page bad_page;
    Status s = bad_page.deserialize(buffer);
    EXPECT_FALSE(s.is_ok());
    EXPECT_EQ(s.code(), StatusCode::InvalidMagic);
}

void test_unsupported_version_rejection() {
    Page page(6, PageType::Data);
    std::array<uint8_t, PAGE_SIZE> buffer{};
    EXPECT_TRUE(page.serialize(buffer).is_ok());

    // Change version to unsupported 99
    encode_u16_be(99, buffer.data() + OFFSET_PAGE_VERSION);

    // Recompute valid checksum for tampered buffer
    encode_u32_be(0, buffer.data() + OFFSET_PAGE_CHECKSUM);
    uint32_t new_crc = calculate_crc32(buffer);
    encode_u32_be(new_crc, buffer.data() + OFFSET_PAGE_CHECKSUM);

    Page bad_page;
    Status s = bad_page.deserialize(buffer);
    EXPECT_FALSE(s.is_ok());
    EXPECT_EQ(s.code(), StatusCode::UnsupportedVersion);
}

int main() {
    std::cout << "Running Page Unit Tests...\n";
    run_test("Endian Roundtrip", test_endian_roundtrip);
    run_test("Checksum Properties", test_checksum_properties);
    run_test("Page Initialization", test_page_initialization);
    run_test("Page Serialization and Deserialization", test_page_serialization_deserialization);
    run_test("Empty Payload", test_empty_payload);
    run_test("Maximum Payload", test_maximum_payload);
    run_test("Payload Overflow Rejection", test_payload_overflow_rejection);
    run_test("Corrupted Checksum Rejection", test_corrupted_checksum_rejection);
    run_test("Incorrect Magic Rejection", test_incorrect_magic_rejection);
    run_test("Unsupported Version Rejection", test_unsupported_version_rejection);

    std::cout << "\nResults: " << g_passed_tests << " passed, " << g_failed_tests << " failed.\n";
    return g_failed_tests == 0 ? 0 : 1;
}
