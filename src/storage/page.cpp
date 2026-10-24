#include "klyrdb/storage/page.hpp"
#include "klyrdb/common/endian.hpp"
#include "klyrdb/common/checksum.hpp"
#include <cstring>
#include <algorithm>

namespace klyrdb {

Page::Page() noexcept {
    header_.magic = PAGE_MAGIC;
    header_.version = PAGE_FORMAT_VERSION;
    header_.page_type = PageType::Data;
    header_.page_id = INVALID_PAGE_ID;
    header_.payload_size = 0;
    header_.flags = 0;
    header_.checksum = 0;
    header_.page_lsn = 0;
    data_.fill(0);
    sync_header_to_raw();
}

Page::Page(page_id_t page_id, PageType type) noexcept : Page() {
    header_.page_id = page_id;
    header_.page_type = type;
    sync_header_to_raw();
}

std::span<const uint8_t> Page::payload() const noexcept {
    return std::span<const uint8_t>(data_.data() + OFFSET_PAGE_PAYLOAD, header_.payload_size);
}

Status Page::set_payload(std::span<const uint8_t> data) noexcept {
    if (data.size() > MAX_PAYLOAD_SIZE) {
        return Status::payload_overflow("Requested payload size " + std::to_string(data.size()) +
                                        " exceeds maximum page capacity " +
                                        std::to_string(MAX_PAYLOAD_SIZE));
    }

    if (!data.empty()) {
        std::memcpy(data_.data() + OFFSET_PAGE_PAYLOAD, data.data(), data.size());
    }

    // Zero out any previously used payload bytes beyond the new payload
    if (data.size() < MAX_PAYLOAD_SIZE) {
        std::memset(data_.data() + OFFSET_PAGE_PAYLOAD + data.size(), 0, MAX_PAYLOAD_SIZE - data.size());
    }

    header_.payload_size = static_cast<uint16_t>(data.size());
    sync_header_to_raw();
    return Status::ok();
}

Status Page::write_payload(size_t offset, std::span<const uint8_t> data) noexcept {
    if (offset > MAX_PAYLOAD_SIZE) {
        return Status::invalid_offset("Offset " + std::to_string(offset) +
                                      " is beyond maximum payload area " +
                                      std::to_string(MAX_PAYLOAD_SIZE));
    }
    if (offset + data.size() > MAX_PAYLOAD_SIZE) {
        return Status::payload_overflow("Writing " + std::to_string(data.size()) +
                                        " bytes at offset " + std::to_string(offset) +
                                        " exceeds maximum payload area " +
                                        std::to_string(MAX_PAYLOAD_SIZE));
    }

    if (!data.empty()) {
        std::memcpy(data_.data() + OFFSET_PAGE_PAYLOAD + offset, data.data(), data.size());
        const size_t end_offset = offset + data.size();
        if (end_offset > header_.payload_size) {
            header_.payload_size = static_cast<uint16_t>(end_offset);
        }
    }
    sync_header_to_raw();
    return Status::ok();
}

void Page::clear_payload() noexcept {
    std::memset(data_.data() + OFFSET_PAGE_PAYLOAD, 0, MAX_PAYLOAD_SIZE);
    header_.payload_size = 0;
    sync_header_to_raw();
}

Status Page::serialize(std::span<uint8_t, PAGE_SIZE> dest) noexcept {
    Status val_status = header_.validate();
    if (!val_status.is_ok()) {
        return val_status;
    }

    // Copy entire internal buffer (payload + zero padding) to dest
    std::memcpy(dest.data(), data_.data(), PAGE_SIZE);

    // Prepare header with checksum field = 0 for checksum computation
    PageHeader hdr_for_checksum = header_;
    hdr_for_checksum.checksum = 0;
    hdr_for_checksum.encode(std::span<uint8_t, PAGE_HEADER_SIZE>(dest.data(), PAGE_HEADER_SIZE));

    // Calculate CRC-32 over the entire 4096-byte page
    const uint32_t computed_crc = calculate_crc32(dest);

    // Store the computed checksum in both the serialized buffer and in-memory header
    encode_u32_be(computed_crc, dest.data() + OFFSET_PAGE_CHECKSUM);
    header_.checksum = computed_crc;

    // Keep internal buffer synchronized
    std::memcpy(data_.data(), dest.data(), PAGE_SIZE);

    return Status::ok();
}

Status Page::deserialize(std::span<const uint8_t, PAGE_SIZE> src) noexcept {
    // 1. Read stored checksum
    const uint32_t stored_checksum = decode_u32_be(src.data() + OFFSET_PAGE_CHECKSUM);

    // 2. Prepare 4KB temporary buffer to verify checksum with checksum field zeroed
    std::array<uint8_t, PAGE_SIZE> verify_buf;
    std::memcpy(verify_buf.data(), src.data(), PAGE_SIZE);
    encode_u32_be(0, verify_buf.data() + OFFSET_PAGE_CHECKSUM);

    const uint32_t computed_checksum = calculate_crc32(verify_buf);
    if (computed_checksum != stored_checksum) {
        return Status::checksum_mismatch("Page checksum mismatch: stored=0x" +
                                         [&] {
                                             char buf[16];
                                             snprintf(buf, sizeof(buf), "%08X", stored_checksum);
                                             return std::string(buf);
                                         }() +
                                         ", calculated=0x" +
                                         [&] {
                                             char buf[16];
                                             snprintf(buf, sizeof(buf), "%08X", computed_checksum);
                                             return std::string(buf);
                                         }());
    }

    // 3. Decode and validate header
    std::span<const uint8_t, PAGE_HEADER_SIZE> hdr_span(src.data(), PAGE_HEADER_SIZE);
    auto hdr_res = PageHeader::decode(hdr_span);
    if (hdr_res.is_err()) {
        return hdr_res.status();
    }

    const PageHeader& hdr = hdr_res.value();
    if (hdr.payload_size > MAX_PAYLOAD_SIZE) {
        return Status::payload_overflow("Deserialized payload size " +
                                        std::to_string(hdr.payload_size) +
                                        " exceeds maximum allowed " +
                                        std::to_string(MAX_PAYLOAD_SIZE));
    }

    // 4. Accept page and copy data
    header_ = hdr;
    std::memcpy(data_.data(), src.data(), PAGE_SIZE);

    return Status::ok();
}

void Page::load_from_raw(std::span<const uint8_t, PAGE_SIZE> src) noexcept {
    std::memcpy(data_.data(), src.data(), PAGE_SIZE);
    std::span<const uint8_t, PAGE_HEADER_SIZE> hdr_span(src.data(), PAGE_HEADER_SIZE);
    auto hdr_res = PageHeader::decode(hdr_span);
    if (hdr_res.is_ok()) {
        header_ = hdr_res.value();
    }
}

} // namespace klyrdb
