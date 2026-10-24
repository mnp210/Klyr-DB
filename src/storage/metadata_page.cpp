#include "klyrdb/storage/metadata_page.hpp"
#include "klyrdb/common/endian.hpp"
#include <array>

namespace klyrdb {

DatabaseMetadata::DatabaseMetadata() noexcept {
    const auto now = std::chrono::system_clock::now();
    created_at = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count()
    );
}

Status DatabaseMetadata::validate() const noexcept {
    if (db_magic != DB_MAGIC) {
        return Status::invalid_magic("Invalid database magic signature: expected 0x4B444231 ('KDB1')");
    }
    if (db_version != DB_FORMAT_VERSION) {
        return Status::unsupported_version("Unsupported database format version: expected " +
                                           std::to_string(DB_FORMAT_VERSION) +
                                           ", got " + std::to_string(db_version));
    }
    if (page_size != PAGE_SIZE) {
        return Status::invalid_argument("Database page size mismatch: expected " +
                                        std::to_string(PAGE_SIZE) +
                                        ", got " + std::to_string(page_size));
    }
    if (page_count < 1) {
        return Status::page_corrupted("Invalid database page count: must be at least 1 (Page 0)");
    }
    return Status::ok();
}

Status DatabaseMetadata::encode_to_page(Page& page) const noexcept {
    Status val_status = validate();
    if (!val_status.is_ok()) {
        return val_status;
    }

    page.set_page_id(0);
    page.set_page_type(PageType::Metadata);

    std::array<uint8_t, DB_HEADER_PAYLOAD_SIZE> payload{};
    encode_u32_be(db_magic, payload.data() + OFFSET_DB_MAGIC);
    encode_u16_be(db_version, payload.data() + OFFSET_DB_VERSION);
    encode_u16_be(page_size, payload.data() + OFFSET_DB_PAGE_SIZE);
    encode_u32_be(page_count, payload.data() + OFFSET_DB_PAGE_COUNT);
    encode_u32_be(free_list_head, payload.data() + OFFSET_DB_FREE_LIST_HEAD);
    encode_u32_be(user_version, payload.data() + OFFSET_DB_USER_VERSION);
    encode_u64_be(created_at, payload.data() + OFFSET_DB_CREATED_AT);
    encode_u32_be(fsm_page_id, payload.data() + OFFSET_DB_FSM_PAGE);
    encode_u32_be(catalog_page_id, payload.data() + OFFSET_DB_CATALOG_PAGE);
    encode_u32_be(table_catalog_page_id, payload.data() + OFFSET_DB_TABLE_CATALOG_PAGE);
    encode_u64_be(checkpoint_lsn, payload.data() + OFFSET_DB_CHECKPOINT_LSN);

    return page.set_payload(payload);
}

Result<DatabaseMetadata> DatabaseMetadata::decode_from_page(const Page& page) noexcept {
    if (page.page_id() != 0) {
        return Status::invalid_page_id("Metadata page must have page_id = 0, got " +
                                       std::to_string(page.page_id()));
    }
    if (page.page_type() != PageType::Metadata) {
        return Status::page_corrupted("Expected PageType::Metadata for Page 0");
    }
    if (page.payload_size() < DB_HEADER_PAYLOAD_SIZE) {
        return Status::page_corrupted("Page 0 payload is too small for database metadata: " +
                                      std::to_string(page.payload_size()) + " < " +
                                      std::to_string(DB_HEADER_PAYLOAD_SIZE));
    }

    auto payload = page.payload();
    DatabaseMetadata meta;
    meta.db_magic        = decode_u32_be(payload.data() + OFFSET_DB_MAGIC);
    meta.db_version      = decode_u16_be(payload.data() + OFFSET_DB_VERSION);
    meta.page_size       = decode_u16_be(payload.data() + OFFSET_DB_PAGE_SIZE);
    meta.page_count      = decode_u32_be(payload.data() + OFFSET_DB_PAGE_COUNT);
    meta.free_list_head  = decode_u32_be(payload.data() + OFFSET_DB_FREE_LIST_HEAD);
    meta.user_version    = decode_u32_be(payload.data() + OFFSET_DB_USER_VERSION);
    meta.created_at      = decode_u64_be(payload.data() + OFFSET_DB_CREATED_AT);
    meta.fsm_page_id           = decode_u32_be(payload.data() + OFFSET_DB_FSM_PAGE);
    meta.catalog_page_id       = decode_u32_be(payload.data() + OFFSET_DB_CATALOG_PAGE);
    meta.table_catalog_page_id = decode_u32_be(payload.data() + OFFSET_DB_TABLE_CATALOG_PAGE);
    meta.checkpoint_lsn        = decode_u64_be(payload.data() + OFFSET_DB_CHECKPOINT_LSN);

    Status val_status = meta.validate();
    if (!val_status.is_ok()) {
        return val_status;
    }

    return meta;
}

} // namespace klyrdb
