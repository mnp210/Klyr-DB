#pragma once

#include <cstdint>
#include <string_view>

namespace klyrdb {

/// Logical page identifier within a database file.
/// Page 0 is reserved for the database header/metadata page.
using page_id_t = uint32_t;

/// Slot identifier within a slotted page.
using slot_id_t = uint16_t;

/// Log Sequence Number for future write-ahead logging (reserved in Phase 1).
using lsn_t = uint64_t;

/// Sentinel value indicating an invalid or unallocated page identifier.
inline constexpr page_id_t INVALID_PAGE_ID = 0xFFFFFFFFU;

/// Sentinel value indicating an invalid or unallocated slot identifier.
inline constexpr slot_id_t INVALID_SLOT_ID = 0xFFFFU;

/// Page classification types.
enum class PageType : uint16_t {
    Invalid       = 0x0000,
    Metadata      = 0x0001, ///< Page 0 database header and system metadata
    Data          = 0x0002, ///< General data payload page
    Record        = 0x0003, ///< Slotted / encoded records page
    Free          = 0x0004, ///< Unallocated / free list page
    BTreeLeaf     = 0x0005, ///< B+ tree leaf node
    BTreeInternal = 0x0006, ///< B+ tree internal node
    FreeSpaceMap  = 0x0007, ///< Free Space Map page
    IndexCatalog  = 0x0008, ///< Index catalog / root tracker page
    TableCatalog  = 0x0009  ///< Table catalog / schema tracker page
};

/// Convert PageType to a human-readable string representation.
[[nodiscard]] constexpr std::string_view page_type_to_string(PageType type) noexcept {
    switch (type) {
        case PageType::Metadata:      return "Metadata";
        case PageType::Data:          return "Data";
        case PageType::Record:        return "Record";
        case PageType::Free:          return "Free";
        case PageType::BTreeLeaf:     return "BTreeLeaf";
        case PageType::BTreeInternal: return "BTreeInternal";
        case PageType::FreeSpaceMap:  return "FreeSpaceMap";
        case PageType::IndexCatalog:  return "IndexCatalog";
        case PageType::TableCatalog:  return "TableCatalog";
        case PageType::Invalid:
        default:                      return "Invalid";
    }
}

template <typename OStream>
inline OStream& operator<<(OStream& os, PageType type) {
    return os << page_type_to_string(type);
}

} // namespace klyrdb
