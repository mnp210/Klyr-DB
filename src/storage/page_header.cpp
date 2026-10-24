#include "klyrdb/storage/page_header.hpp"
#include "klyrdb/common/endian.hpp"

namespace klyrdb {

Status PageHeader::validate() const noexcept {
    if (magic != PAGE_MAGIC) {
        return Status::invalid_magic("Invalid page magic signature: expected 0x4B4C5952 ('KLYR')");
    }
    if (version != PAGE_FORMAT_VERSION) {
        return Status::unsupported_version("Unsupported page version: expected " +
                                           std::to_string(PAGE_FORMAT_VERSION) +
                                           ", got " + std::to_string(version));
    }
    if (page_type != PageType::Metadata &&
        page_type != PageType::Data &&
        page_type != PageType::Record &&
        page_type != PageType::Free &&
        page_type != PageType::BTreeLeaf &&
        page_type != PageType::BTreeInternal &&
        page_type != PageType::FreeSpaceMap &&
        page_type != PageType::IndexCatalog &&
        page_type != PageType::TableCatalog) {
        return Status::page_corrupted("Unknown page type: " +
                                      std::to_string(static_cast<uint16_t>(page_type)));
    }
    if (payload_size > MAX_PAYLOAD_SIZE) {
        return Status::payload_overflow("Payload size exceeds maximum allowed capacity: " +
                                        std::to_string(payload_size) + " > " +
                                        std::to_string(MAX_PAYLOAD_SIZE));
    }
    return Status::ok();
}

void PageHeader::encode(std::span<uint8_t, PAGE_HEADER_SIZE> dest) const noexcept {
    encode_u32_be(magic, dest.data() + OFFSET_PAGE_MAGIC);
    encode_u16_be(version, dest.data() + OFFSET_PAGE_VERSION);
    encode_u16_be(static_cast<uint16_t>(page_type), dest.data() + OFFSET_PAGE_TYPE);
    encode_u32_be(page_id, dest.data() + OFFSET_PAGE_ID);
    encode_u16_be(payload_size, dest.data() + OFFSET_PAGE_PAYLOAD_SIZE);
    encode_u16_be(flags, dest.data() + OFFSET_PAGE_FLAGS);
    encode_u32_be(checksum, dest.data() + OFFSET_PAGE_CHECKSUM);
    encode_u32_be(page_lsn, dest.data() + OFFSET_PAGE_LSN);
}

Result<PageHeader> PageHeader::decode(std::span<const uint8_t, PAGE_HEADER_SIZE> src) noexcept {
    PageHeader hdr;
    hdr.magic        = decode_u32_be(src.data() + OFFSET_PAGE_MAGIC);
    hdr.version      = decode_u16_be(src.data() + OFFSET_PAGE_VERSION);
    hdr.page_type    = static_cast<PageType>(decode_u16_be(src.data() + OFFSET_PAGE_TYPE));
    hdr.page_id      = decode_u32_be(src.data() + OFFSET_PAGE_ID);
    hdr.payload_size = decode_u16_be(src.data() + OFFSET_PAGE_PAYLOAD_SIZE);
    hdr.flags        = decode_u16_be(src.data() + OFFSET_PAGE_FLAGS);
    hdr.checksum     = decode_u32_be(src.data() + OFFSET_PAGE_CHECKSUM);
    hdr.page_lsn     = decode_u32_be(src.data() + OFFSET_PAGE_LSN);

    Status status = hdr.validate();
    if (!status.is_ok()) {
        return status;
    }

    return hdr;
}

} // namespace klyrdb
