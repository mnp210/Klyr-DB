#pragma once

#include <cstdint>
#include <cstddef>

namespace klyrdb {

/// The fixed size of every page in KlyrDB (4096 bytes / 4 KiB).
inline constexpr size_t PAGE_SIZE = 4096;

/// Page Magic signature: ASCII "KLYR" (0x4B4C5952).
inline constexpr uint32_t PAGE_MAGIC = 0x4B4C5952U;

/// Page format specification version (Phase 1).
inline constexpr uint16_t PAGE_FORMAT_VERSION = 1;

/// Fixed page header size in bytes.
inline constexpr size_t PAGE_HEADER_SIZE = 24;

/// Maximum payload bytes that can fit into a single page.
inline constexpr size_t MAX_PAYLOAD_SIZE = PAGE_SIZE - PAGE_HEADER_SIZE; // 4072 bytes

// --- Exact Page Header Byte Offsets ---
inline constexpr size_t OFFSET_PAGE_MAGIC        = 0;  // 4 bytes: uint32_t
inline constexpr size_t OFFSET_PAGE_VERSION      = 4;  // 2 bytes: uint16_t
inline constexpr size_t OFFSET_PAGE_TYPE         = 6;  // 2 bytes: uint16_t
inline constexpr size_t OFFSET_PAGE_ID           = 8;  // 4 bytes: uint32_t
inline constexpr size_t OFFSET_PAGE_PAYLOAD_SIZE = 12; // 2 bytes: uint16_t
inline constexpr size_t OFFSET_PAGE_FLAGS        = 14; // 2 bytes: uint16_t
inline constexpr size_t OFFSET_PAGE_CHECKSUM     = 16; // 4 bytes: uint32_t
inline constexpr size_t OFFSET_PAGE_LSN          = 20; // 4 bytes: uint32_t (Page LSN)
inline constexpr size_t OFFSET_PAGE_RESERVED     = 20; // 4 bytes: alias for backwards compatibility
inline constexpr size_t OFFSET_PAGE_PAYLOAD      = 24; // 4072 bytes

// --- Database Header (Page 0 Payload) Constants ---
inline constexpr uint32_t DB_MAGIC = 0x4B444231U; // ASCII "KDB1"
inline constexpr uint16_t DB_FORMAT_VERSION = 1;
inline constexpr size_t DB_HEADER_PAYLOAD_SIZE = 48;

inline constexpr size_t OFFSET_DB_MAGIC          = 0;  // 4 bytes: uint32_t
inline constexpr size_t OFFSET_DB_VERSION        = 4;  // 2 bytes: uint16_t
inline constexpr size_t OFFSET_DB_PAGE_SIZE      = 6;  // 2 bytes: uint16_t
inline constexpr size_t OFFSET_DB_PAGE_COUNT     = 8;  // 4 bytes: uint32_t
inline constexpr size_t OFFSET_DB_FREE_LIST_HEAD = 12; // 4 bytes: uint32_t
inline constexpr size_t OFFSET_DB_USER_VERSION   = 16; // 4 bytes: uint32_t
inline constexpr size_t OFFSET_DB_CREATED_AT     = 20; // 8 bytes: uint64_t
inline constexpr size_t OFFSET_DB_FSM_PAGE           = 28; // 4 bytes: uint32_t (FSM Page ID, INVALID_PAGE_ID if unassigned)
inline constexpr size_t OFFSET_DB_CATALOG_PAGE       = 32; // 4 bytes: uint32_t (Index Catalog Page ID, INVALID_PAGE_ID if unassigned)
inline constexpr size_t OFFSET_DB_TABLE_CATALOG_PAGE = 36; // 4 bytes: uint32_t (Table Catalog Page ID, INVALID_PAGE_ID if unassigned)
inline constexpr size_t OFFSET_DB_CHECKPOINT_LSN     = 40; // 8 bytes: uint64_t (Durable Checkpoint LSN)
inline constexpr size_t OFFSET_DB_RESERVED           = 40; // 8 bytes: alias for backwards compatibility

} // namespace klyrdb
