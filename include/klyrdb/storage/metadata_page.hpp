#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/storage/page_constants.hpp"
#include "klyrdb/storage/page.hpp"
#include <chrono>

namespace klyrdb {

/// Logical representation of the KlyrDB Database Metadata stored in Page 0.
struct DatabaseMetadata {
    uint32_t db_magic{DB_MAGIC};
    uint16_t db_version{DB_FORMAT_VERSION};
    uint16_t page_size{static_cast<uint16_t>(PAGE_SIZE)};
    uint32_t page_count{1}; // At least Page 0
    page_id_t free_list_head{INVALID_PAGE_ID};
    uint32_t user_version{0};
    uint64_t created_at{0};
    page_id_t fsm_page_id{INVALID_PAGE_ID};
    page_id_t catalog_page_id{INVALID_PAGE_ID};
    page_id_t table_catalog_page_id{INVALID_PAGE_ID};
    lsn_t checkpoint_lsn{0};

    DatabaseMetadata() noexcept;

    [[nodiscard]] Status validate() const noexcept;

    /// Encodes metadata into the payload of a Page (which must be Page 0).
    Status encode_to_page(Page& page) const noexcept;

    /// Decodes metadata from a Page (which must be Page 0).
    [[nodiscard]] static Result<DatabaseMetadata> decode_from_page(const Page& page) noexcept;
};

} // namespace klyrdb
