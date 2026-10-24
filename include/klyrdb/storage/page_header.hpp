#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/storage/page_constants.hpp"
#include <span>

namespace klyrdb {

/// Logical representation of the 24-byte fixed page header.
struct PageHeader {
    uint32_t magic{PAGE_MAGIC};
    uint16_t version{PAGE_FORMAT_VERSION};
    PageType page_type{PageType::Data};
    page_id_t page_id{INVALID_PAGE_ID};
    uint16_t payload_size{0};
    uint16_t flags{0};
    uint32_t checksum{0};
    uint32_t page_lsn{0}; ///< Page LSN (was reserved in Phase 1-4)

    // Backward-compatible alias
    [[nodiscard]] uint32_t reserved() const noexcept { return page_lsn; }
    void set_reserved(uint32_t r) noexcept { page_lsn = r; }

    /// Validates the structural integrity and bounds of the header fields.
    [[nodiscard]] Status validate() const noexcept;

    /// Encodes the header fields explicitly into big-endian bytes.
    void encode(std::span<uint8_t, PAGE_HEADER_SIZE> dest) const noexcept;

    /// Decodes and validates header fields from a 24-byte slice.
    [[nodiscard]] static Result<PageHeader> decode(std::span<const uint8_t, PAGE_HEADER_SIZE> src) noexcept;
};

} // namespace klyrdb
