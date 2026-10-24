#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/common/endian.hpp"
#include "klyrdb/storage/page_constants.hpp"
#include <cstring>

namespace klyrdb {

namespace {

constexpr size_t OFFSET_CAT_TYPE     = 0;
constexpr size_t OFFSET_CAT_COUNT    = 2;
constexpr size_t OFFSET_CAT_RESERVED = 4;
constexpr size_t OFFSET_CAT_ENTRIES  = 8;

constexpr size_t CAT_ENTRY_SIZE      = 72;
constexpr size_t CAT_NAME_LEN        = 64;
constexpr size_t MAX_CATALOG_ENTRIES = (MAX_PAYLOAD_SIZE - OFFSET_CAT_ENTRIES) / CAT_ENTRY_SIZE;

} // namespace

Result<std::unique_ptr<IndexCatalog>> IndexCatalog::open_or_create(BufferPoolManager& bpm) {
    page_id_t cat_pid = bpm.page_file().metadata().catalog_page_id;

    if (cat_pid == INVALID_PAGE_ID) {
        // Allocate new catalog page
        auto guard_res = bpm.new_page_guard(cat_pid);
        if (guard_res.is_err()) {
            return guard_res.status();
        }

        auto& guard = guard_res.value();
        guard->set_page_type(PageType::IndexCatalog);
        auto p = guard->mutable_payload();
        std::memset(p.data(), 0, MAX_PAYLOAD_SIZE);

        encode_u16_be(static_cast<uint16_t>(PageType::IndexCatalog), p.data() + OFFSET_CAT_TYPE);
        encode_u16_be(0, p.data() + OFFSET_CAT_COUNT);
        guard->set_payload_size(static_cast<uint16_t>(MAX_PAYLOAD_SIZE));
        guard.mark_dirty();

        // Update database metadata on Page 0 with freshly updated metadata
        auto meta = bpm.page_file().metadata();
        meta.catalog_page_id = cat_pid;
        Status update_st = bpm.page_file().update_metadata(meta);
        if (!update_st.is_ok()) {
            return update_st;
        }
    } else {
        auto guard_res = bpm.fetch_page_guard(cat_pid);
        if (guard_res.is_err()) {
            return guard_res.status();
        }
        if (guard_res.value()->page_type() != PageType::IndexCatalog) {
            return Status::page_corrupted("Page " + std::to_string(cat_pid) + " is not an IndexCatalog page");
        }
    }

    return std::make_unique<IndexCatalog>(bpm, cat_pid);
}

Result<IndexMetadata> IndexCatalog::get_index(std::string_view name) {
    auto guard_res = bpm_.fetch_page_guard(catalog_page_id_);
    if (guard_res.is_err()) return guard_res.status();

    auto& guard = guard_res.value();
    const uint8_t* p = guard->payload().data();
    const uint16_t count = decode_u16_be(p + OFFSET_CAT_COUNT);

    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* entry = p + OFFSET_CAT_ENTRIES + (i * CAT_ENTRY_SIZE);
        char entry_name[CAT_NAME_LEN + 1]{};
        std::memcpy(entry_name, entry, CAT_NAME_LEN);

        if (name == entry_name) {
            IndexMetadata info;
            info.name = entry_name;
            info.root_page_id = decode_u32_be(entry + 64);
            info.key_type = static_cast<KeyType>(entry[68]);
            info.is_unique = (entry[69] != 0);
            return info;
        }
    }

    return Status::index_not_found("Index '" + std::string(name) + "' not found in catalog");
}

Status IndexCatalog::create_index(std::string_view name, KeyType key_type, page_id_t root_page_id, bool is_unique) {
    if (name.empty() || name.size() >= CAT_NAME_LEN) {
        return Status::invalid_argument("Index name is empty or exceeds " + std::to_string(CAT_NAME_LEN - 1) + " characters");
    }

    auto guard_res = bpm_.fetch_page_guard(catalog_page_id_);
    if (guard_res.is_err()) return guard_res.status();

    auto& guard = guard_res.value();
    uint8_t* p = guard->mutable_payload().data();
    const uint16_t count = decode_u16_be(p + OFFSET_CAT_COUNT);

    // Verify uniqueness of name
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* entry = p + OFFSET_CAT_ENTRIES + (i * CAT_ENTRY_SIZE);
        char entry_name[CAT_NAME_LEN + 1]{};
        std::memcpy(entry_name, entry, CAT_NAME_LEN);
        if (name == entry_name) {
            return Status::duplicate_key("Index with name '" + std::string(name) + "' already exists");
        }
    }

    if (count >= MAX_CATALOG_ENTRIES) {
        return Status::no_free_space("IndexCatalog is full (maximum " + std::to_string(MAX_CATALOG_ENTRIES) + " indexes)");
    }

    uint8_t* new_entry = p + OFFSET_CAT_ENTRIES + (count * CAT_ENTRY_SIZE);
    std::memset(new_entry, 0, CAT_ENTRY_SIZE);

    std::memcpy(new_entry, name.data(), name.size());
    encode_u32_be(root_page_id, new_entry + 64);
    new_entry[68] = static_cast<uint8_t>(key_type);
    new_entry[69] = is_unique ? 1 : 0;

    encode_u16_be(count + 1, p + OFFSET_CAT_COUNT);
    guard.mark_dirty();
    return Status::ok();
}

Status IndexCatalog::update_root_page(std::string_view name, page_id_t new_root_id) {
    auto guard_res = bpm_.fetch_page_guard(catalog_page_id_);
    if (guard_res.is_err()) return guard_res.status();

    auto& guard = guard_res.value();
    uint8_t* p = guard->mutable_payload().data();
    const uint16_t count = decode_u16_be(p + OFFSET_CAT_COUNT);

    for (uint16_t i = 0; i < count; ++i) {
        uint8_t* entry = p + OFFSET_CAT_ENTRIES + (i * CAT_ENTRY_SIZE);
        char entry_name[CAT_NAME_LEN + 1]{};
        std::memcpy(entry_name, entry, CAT_NAME_LEN);

        if (name == entry_name) {
            encode_u32_be(new_root_id, entry + 64);
            guard.mark_dirty();
            return Status::ok();
        }
    }

    return Status::index_not_found("Index '" + std::string(name) + "' not found in catalog");
}

Status IndexCatalog::drop_index(std::string_view name) {
    auto guard_res = bpm_.fetch_page_guard(catalog_page_id_);
    if (guard_res.is_err()) return guard_res.status();

    auto& guard = guard_res.value();
    uint8_t* p = guard->mutable_payload().data();
    const uint16_t count = decode_u16_be(p + OFFSET_CAT_COUNT);

    for (uint16_t i = 0; i < count; ++i) {
        uint8_t* entry = p + OFFSET_CAT_ENTRIES + (i * CAT_ENTRY_SIZE);
        char entry_name[CAT_NAME_LEN + 1]{};
        std::memcpy(entry_name, entry, CAT_NAME_LEN);

        if (name == entry_name) {
            // Shift subsequent entries down
            for (uint16_t j = i; j + 1 < count; ++j) {
                uint8_t* dest = p + OFFSET_CAT_ENTRIES + (j * CAT_ENTRY_SIZE);
                const uint8_t* src = p + OFFSET_CAT_ENTRIES + ((j + 1) * CAT_ENTRY_SIZE);
                std::memcpy(dest, src, CAT_ENTRY_SIZE);
            }
            encode_u16_be(count - 1, p + OFFSET_CAT_COUNT);
            guard.mark_dirty();
            return Status::ok();
        }
    }

    return Status::index_not_found("Index '" + std::string(name) + "' not found in catalog");
}

Result<std::vector<IndexMetadata>> IndexCatalog::list_indexes() {
    auto guard_res = bpm_.fetch_page_guard(catalog_page_id_);
    if (guard_res.is_err()) return guard_res.status();

    auto& guard = guard_res.value();
    const uint8_t* p = guard->payload().data();
    const uint16_t count = decode_u16_be(p + OFFSET_CAT_COUNT);

    std::vector<IndexMetadata> list;
    list.reserve(count);

    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* entry = p + OFFSET_CAT_ENTRIES + (i * CAT_ENTRY_SIZE);
        char entry_name[CAT_NAME_LEN + 1]{};
        std::memcpy(entry_name, entry, CAT_NAME_LEN);

        IndexMetadata info;
        info.name = entry_name;
        info.root_page_id = decode_u32_be(entry + 64);
        info.key_type = static_cast<KeyType>(entry[68]);
        info.is_unique = (entry[69] != 0);
        list.push_back(std::move(info));
    }

    return list;
}

} // namespace klyrdb
