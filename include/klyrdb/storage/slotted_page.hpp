#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/storage/page_constants.hpp"
#include "klyrdb/storage/page.hpp"
#include <span>

namespace klyrdb {

/// Slotted Page Constants (within the 4072-byte usable payload area)
inline constexpr size_t SLOTTED_PAGE_HEADER_SIZE = 8;
inline constexpr size_t SLOT_ENTRY_SIZE          = 4;

inline constexpr size_t OFFSET_SLOT_COUNT          = 0; // 2 bytes: uint16_t
inline constexpr size_t OFFSET_FREE_SPACE_PTR      = 2; // 2 bytes: uint16_t
inline constexpr size_t OFFSET_DELETED_SLOT_COUNT  = 4; // 2 bytes: uint16_t
inline constexpr size_t OFFSET_SLOTTED_FLAGS       = 6; // 2 bytes: uint16_t
inline constexpr size_t OFFSET_SLOT_DIRECTORY      = 8; // slot entries start here

inline constexpr uint16_t SLOT_DELETED_OFFSET = 0xFFFFU;
inline constexpr uint16_t SLOT_DELETED_LENGTH = 0U;

/// Maximum single record payload size in an empty slotted page (4060 bytes)
inline constexpr size_t MAX_SLOTTED_RECORD_SIZE =
    MAX_PAYLOAD_SIZE - SLOTTED_PAGE_HEADER_SIZE - SLOT_ENTRY_SIZE;

/// Slotted page abstraction for managing multiple variable-length records
/// within a single 4096-byte database page.
///
/// Layout inside 4072-byte payload area:
/// [0..7]:   Slotted page header (slot_count, free_space_ptr, deleted_count, flags)
/// [8..N]:   Slot directory growing upwards (4 bytes per slot: offset, length)
/// [N..FSP]: Contiguous free space
/// [FSP..4071]: Record data growing downwards from payload end
class SlottedPage {
public:
    explicit SlottedPage(Page& page) noexcept : page_(page) {}

    /// Formats the page as a valid empty slotted page (setting PageType::Record).
    static Status initialize(Page& page) noexcept;

    /// Checks if a page has valid slotted page headers.
    static bool is_slotted_page(const Page& page) noexcept;

    /// Inserts a variable-length record, reusing a deleted slot or appending a new one.
    /// Returns the assigned slot ID on success.
    Result<slot_id_t> insert_record(std::span<const uint8_t> data);

    /// Retrieves a read-only view of a record by slot ID.
    [[nodiscard]] Result<std::span<const uint8_t>> get_record(slot_id_t slot_id) const;

    /// Updates an existing record's contents.
    Status update_record(slot_id_t slot_id, std::span<const uint8_t> new_data);

    /// Deletes a record, marking its slot as a tombstone.
    Status delete_record(slot_id_t slot_id);

    /// Compacts the page by defragmenting active records towards the end of the page.
    /// Slot IDs remain completely stable and unchanged.
    Status compact();

    /// Returns the number of slots in the directory (including deleted slots).
    [[nodiscard]] uint16_t slot_count() const noexcept;

    /// Returns the number of tombstoned (deleted) slots in the directory.
    [[nodiscard]] uint16_t deleted_slot_count() const noexcept;

    /// Returns the contiguous free space in bytes between the slot directory and free_space_pointer.
    [[nodiscard]] uint16_t contiguous_free_space() const noexcept;

    /// Returns the total free space in bytes (contiguous + fragmented dead space).
    [[nodiscard]] uint16_t total_free_space() const noexcept;

    /// Checks if the page can accommodate a record of the specified size.
    [[nodiscard]] bool can_insert(size_t record_size) const noexcept;

    [[nodiscard]] Page& page() noexcept { return page_; }
    [[nodiscard]] const Page& page() const noexcept { return page_; }

private:
    [[nodiscard]] uint8_t* payload_data() noexcept;
    [[nodiscard]] const uint8_t* payload_data() const noexcept;

    [[nodiscard]] uint16_t get_free_space_pointer() const noexcept;
    void set_free_space_pointer(uint16_t ptr) noexcept;

    void set_slot_count(uint16_t count) noexcept;
    void set_deleted_slot_count(uint16_t count) noexcept;

    struct SlotEntry {
        uint16_t offset{SLOT_DELETED_OFFSET};
        uint16_t length{SLOT_DELETED_LENGTH};
        [[nodiscard]] bool is_deleted() const noexcept { return offset == SLOT_DELETED_OFFSET; }
    };

    [[nodiscard]] SlotEntry get_slot(slot_id_t slot_id) const noexcept;
    void set_slot(slot_id_t slot_id, const SlotEntry& entry) noexcept;

    Page& page_;
};

} // namespace klyrdb
