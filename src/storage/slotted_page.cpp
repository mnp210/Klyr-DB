#include "klyrdb/storage/slotted_page.hpp"
#include "klyrdb/common/endian.hpp"
#include <cstring>
#include <array>

namespace klyrdb {

Status SlottedPage::initialize(Page& page) noexcept {
    page.set_page_type(PageType::Record);
    auto payload = page.mutable_payload();
    std::memset(payload.data(), 0, MAX_PAYLOAD_SIZE);

    encode_u16_be(0, payload.data() + OFFSET_SLOT_COUNT);
    encode_u16_be(static_cast<uint16_t>(MAX_PAYLOAD_SIZE), payload.data() + OFFSET_FREE_SPACE_PTR);
    encode_u16_be(0, payload.data() + OFFSET_DELETED_SLOT_COUNT);
    encode_u16_be(0, payload.data() + OFFSET_SLOTTED_FLAGS);

    page.set_payload_size(static_cast<uint16_t>(MAX_PAYLOAD_SIZE));
    return Status::ok();
}

bool SlottedPage::is_slotted_page(const Page& page) noexcept {
    if (page.page_type() != PageType::Record) {
        return false;
    }
    if (page.payload_size() < SLOTTED_PAGE_HEADER_SIZE) {
        return false;
    }
    const uint8_t* p = page.payload().data();
    const uint16_t count = decode_u16_be(p + OFFSET_SLOT_COUNT);
    const uint16_t fsp = decode_u16_be(p + OFFSET_FREE_SPACE_PTR);
    const size_t dir_end = SLOTTED_PAGE_HEADER_SIZE + (static_cast<size_t>(count) * SLOT_ENTRY_SIZE);

    return (fsp <= MAX_PAYLOAD_SIZE && dir_end <= fsp);
}

uint8_t* SlottedPage::payload_data() noexcept {
    return page_.mutable_payload().data();
}

const uint8_t* SlottedPage::payload_data() const noexcept {
    return page_.payload().data();
}

uint16_t SlottedPage::slot_count() const noexcept {
    return decode_u16_be(payload_data() + OFFSET_SLOT_COUNT);
}

void SlottedPage::set_slot_count(uint16_t count) noexcept {
    encode_u16_be(count, payload_data() + OFFSET_SLOT_COUNT);
}

uint16_t SlottedPage::deleted_slot_count() const noexcept {
    return decode_u16_be(payload_data() + OFFSET_DELETED_SLOT_COUNT);
}

void SlottedPage::set_deleted_slot_count(uint16_t count) noexcept {
    encode_u16_be(count, payload_data() + OFFSET_DELETED_SLOT_COUNT);
}

uint16_t SlottedPage::get_free_space_pointer() const noexcept {
    return decode_u16_be(payload_data() + OFFSET_FREE_SPACE_PTR);
}

void SlottedPage::set_free_space_pointer(uint16_t ptr) noexcept {
    encode_u16_be(ptr, payload_data() + OFFSET_FREE_SPACE_PTR);
}

SlottedPage::SlotEntry SlottedPage::get_slot(slot_id_t slot_id) const noexcept {
    const size_t offset = OFFSET_SLOT_DIRECTORY + (static_cast<size_t>(slot_id) * SLOT_ENTRY_SIZE);
    SlotEntry entry;
    entry.offset = decode_u16_be(payload_data() + offset);
    entry.length = decode_u16_be(payload_data() + offset + 2);
    return entry;
}

void SlottedPage::set_slot(slot_id_t slot_id, const SlotEntry& entry) noexcept {
    const size_t offset = OFFSET_SLOT_DIRECTORY + (static_cast<size_t>(slot_id) * SLOT_ENTRY_SIZE);
    encode_u16_be(entry.offset, payload_data() + offset);
    encode_u16_be(entry.length, payload_data() + offset + 2);
}

uint16_t SlottedPage::contiguous_free_space() const noexcept {
    const uint16_t fsp = get_free_space_pointer();
    const size_t dir_end = SLOTTED_PAGE_HEADER_SIZE + (static_cast<size_t>(slot_count()) * SLOT_ENTRY_SIZE);
    if (fsp >= dir_end) {
        return static_cast<uint16_t>(fsp - dir_end);
    }
    return 0;
}

uint16_t SlottedPage::total_free_space() const noexcept {
    uint32_t active_bytes = 0;
    const uint16_t count = slot_count();
    for (uint16_t i = 0; i < count; ++i) {
        auto slot = get_slot(i);
        if (!slot.is_deleted()) {
            active_bytes += slot.length;
        }
    }

    const uint32_t total_dir_overhead =
        static_cast<uint32_t>(SLOTTED_PAGE_HEADER_SIZE) + (static_cast<uint32_t>(count) * SLOT_ENTRY_SIZE);

    if (total_dir_overhead + active_bytes >= MAX_PAYLOAD_SIZE) {
        return 0;
    }
    return static_cast<uint16_t>(MAX_PAYLOAD_SIZE - total_dir_overhead - active_bytes);
}

bool SlottedPage::can_insert(size_t record_size) const noexcept {
    if (record_size > MAX_SLOTTED_RECORD_SIZE) {
        return false;
    }
    const size_t needed = record_size + (deleted_slot_count() > 0 ? 0 : SLOT_ENTRY_SIZE);
    return total_free_space() >= needed;
}

Result<slot_id_t> SlottedPage::insert_record(std::span<const uint8_t> data) {
    if (data.size() > MAX_SLOTTED_RECORD_SIZE) {
        return Status::payload_overflow("Record size " + std::to_string(data.size()) +
                                        " exceeds max slotted record size " +
                                        std::to_string(MAX_SLOTTED_RECORD_SIZE));
    }

    const uint16_t count = slot_count();
    bool reusing_slot = false;
    slot_id_t target_slot_id = count;

    // Check for a recyclable deleted slot
    if (deleted_slot_count() > 0) {
        for (uint16_t i = 0; i < count; ++i) {
            if (get_slot(i).is_deleted()) {
                target_slot_id = i;
                reusing_slot = true;
                break;
            }
        }
    }

    const size_t needed_space = data.size() + (reusing_slot ? 0 : SLOT_ENTRY_SIZE);
    if (total_free_space() < needed_space) {
        return Status::no_free_space("Insufficient total free space for record insertion: need " +
                                     std::to_string(needed_space) + ", have " +
                                     std::to_string(total_free_space()));
    }

    // If contiguous free space is not enough, defragment active records first
    if (contiguous_free_space() < needed_space) {
        Status s = compact();
        if (!s.is_ok()) {
            return s;
        }
        if (contiguous_free_space() < needed_space) {
            return Status::no_free_space("Insufficient contiguous space even after compaction");
        }
    }

    const uint16_t current_fsp = get_free_space_pointer();
    const uint16_t new_fsp = static_cast<uint16_t>(current_fsp - data.size());

    if (!data.empty()) {
        std::memcpy(payload_data() + new_fsp, data.data(), data.size());
    }

    SlotEntry entry;
    entry.offset = new_fsp;
    entry.length = static_cast<uint16_t>(data.size());

    set_slot(target_slot_id, entry);
    set_free_space_pointer(new_fsp);

    if (reusing_slot) {
        set_deleted_slot_count(deleted_slot_count() - 1);
    } else {
        set_slot_count(count + 1);
    }

    return target_slot_id;
}

Result<std::span<const uint8_t>> SlottedPage::get_record(slot_id_t slot_id) const {
    if (slot_id >= slot_count()) {
        return Status::invalid_slot_id("Slot ID " + std::to_string(slot_id) +
                                       " is out of range [0, " +
                                       std::to_string(slot_count()) + ")");
    }

    const SlotEntry entry = get_slot(slot_id);
    if (entry.is_deleted()) {
        return Status::record_not_found("Slot " + std::to_string(slot_id) + " is deleted");
    }

    const uint16_t fsp = get_free_space_pointer();
    if (entry.offset < fsp ||
        entry.offset + entry.length > MAX_PAYLOAD_SIZE) {
        return Status::page_corrupted("Corrupted slot bounds for slot " + std::to_string(slot_id) +
                                      ": offset=" + std::to_string(entry.offset) +
                                      ", length=" + std::to_string(entry.length) +
                                      ", fsp=" + std::to_string(fsp));
    }

    return std::span<const uint8_t>(payload_data() + entry.offset, entry.length);
}

Status SlottedPage::update_record(slot_id_t slot_id, std::span<const uint8_t> new_data) {
    if (slot_id >= slot_count()) {
        return Status::invalid_slot_id("Slot ID " + std::to_string(slot_id) +
                                       " is out of range [0, " +
                                       std::to_string(slot_count()) + ")");
    }

    const SlotEntry old_entry = get_slot(slot_id);
    if (old_entry.is_deleted()) {
        return Status::record_not_found("Cannot update deleted slot " + std::to_string(slot_id));
    }

    if (new_data.size() == old_entry.length) {
        if (!new_data.empty()) {
            std::memcpy(payload_data() + old_entry.offset, new_data.data(), new_data.size());
        }
        return Status::ok();
    }

    if (new_data.size() < old_entry.length) {
        if (!new_data.empty()) {
            std::memcpy(payload_data() + old_entry.offset, new_data.data(), new_data.size());
        }
        SlotEntry updated_entry = old_entry;
        updated_entry.length = static_cast<uint16_t>(new_data.size());
        set_slot(slot_id, updated_entry);
        return Status::ok();
    }

    // Growth case: new_data.size() > old_entry.length
    const size_t growth = new_data.size() - old_entry.length;
    if (total_free_space() < growth) {
        return Status::no_free_space("Insufficient free space to grow record: need " +
                                     std::to_string(growth) + ", have " +
                                     std::to_string(total_free_space()));
    }

    // Defragment existing records excluding the old content of this slot
    // To do this safely and cleanly:
    // Temporarily mark the slot as deleted, compact the page to free up all available space,
    // then allocate the new record payload at new_fsp and point the slot to it!
    set_slot(slot_id, {SLOT_DELETED_OFFSET, SLOT_DELETED_LENGTH});
    Status compact_status = compact();
    if (!compact_status.is_ok()) {
        // Rollback on unexpected failure
        set_slot(slot_id, old_entry);
        return compact_status;
    }

    const uint16_t current_fsp = get_free_space_pointer();
    const uint16_t new_fsp = static_cast<uint16_t>(current_fsp - new_data.size());

    if (!new_data.empty()) {
        std::memcpy(payload_data() + new_fsp, new_data.data(), new_data.size());
    }

    SlotEntry updated_entry;
    updated_entry.offset = new_fsp;
    updated_entry.length = static_cast<uint16_t>(new_data.size());

    set_slot(slot_id, updated_entry);
    set_free_space_pointer(new_fsp);
    return Status::ok();
}

Status SlottedPage::delete_record(slot_id_t slot_id) {
    if (slot_id >= slot_count()) {
        return Status::invalid_slot_id("Slot ID " + std::to_string(slot_id) +
                                       " is out of range [0, " +
                                       std::to_string(slot_count()) + ")");
    }

    const SlotEntry entry = get_slot(slot_id);
    if (entry.is_deleted()) {
        return Status::record_not_found("Slot " + std::to_string(slot_id) + " is already deleted");
    }

    set_slot(slot_id, {SLOT_DELETED_OFFSET, SLOT_DELETED_LENGTH});
    set_deleted_slot_count(deleted_slot_count() + 1);
    return Status::ok();
}

Status SlottedPage::compact() {
    const uint16_t count = slot_count();
    std::array<uint8_t, MAX_PAYLOAD_SIZE> scratch{};

    // Copy slotted page header (8 bytes) to scratch
    std::memcpy(scratch.data(), payload_data(), SLOTTED_PAGE_HEADER_SIZE);

    uint16_t new_fsp = static_cast<uint16_t>(MAX_PAYLOAD_SIZE);

    for (uint16_t i = 0; i < count; ++i) {
        const SlotEntry old_entry = get_slot(i);
        const size_t slot_offset_in_scratch =
            OFFSET_SLOT_DIRECTORY + (static_cast<size_t>(i) * SLOT_ENTRY_SIZE);

        if (old_entry.is_deleted()) {
            encode_u16_be(SLOT_DELETED_OFFSET, scratch.data() + slot_offset_in_scratch);
            encode_u16_be(SLOT_DELETED_LENGTH, scratch.data() + slot_offset_in_scratch + 2);
        } else {
            new_fsp = static_cast<uint16_t>(new_fsp - old_entry.length);
            if (old_entry.length > 0) {
                std::memcpy(scratch.data() + new_fsp,
                            payload_data() + old_entry.offset,
                            old_entry.length);
            }
            encode_u16_be(new_fsp, scratch.data() + slot_offset_in_scratch);
            encode_u16_be(old_entry.length, scratch.data() + slot_offset_in_scratch + 2);
        }
    }

    // Update free_space_pointer in scratch header
    encode_u16_be(new_fsp, scratch.data() + OFFSET_FREE_SPACE_PTR);

    // Copy scratch buffer back into page payload
    std::memcpy(payload_data(), scratch.data(), MAX_PAYLOAD_SIZE);

    return Status::ok();
}

} // namespace klyrdb
