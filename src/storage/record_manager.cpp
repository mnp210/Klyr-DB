#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"

namespace klyrdb {

Result<RecordId> RecordManager::insert_record(std::span<const uint8_t> data) {
    if (data.size() > MAX_SLOTTED_RECORD_SIZE) {
        return Status::payload_overflow("Record size " + std::to_string(data.size()) +
                                        " exceeds maximum single page capacity " +
                                        std::to_string(MAX_SLOTTED_RECORD_SIZE));
    }

    // A slot entry takes 4 bytes (2 bytes offset + 2 bytes length)
    const size_t needed_space = data.size() + 4;

    // 1. Fast path: Query Free-Space Map if available
    if (fsm_ != nullptr) {
        auto candidate_res = fsm_->find_page_with_space(needed_space);
        if (candidate_res.is_ok()) {
            page_id_t candidate_pid = candidate_res.value();
            auto guard_res = bpm_.fetch_page_guard(candidate_pid);
            if (guard_res.is_ok()) {
                auto& guard = guard_res.value();
                if (guard->page_type() == PageType::Record && SlottedPage::is_slotted_page(*guard)) {
                    SlottedPage slotted(*guard);
                    if (slotted.can_insert(data.size())) {
                        auto insert_res = slotted.insert_record(data);
                        if (insert_res.is_ok()) {
                            guard.mark_dirty();
                            fsm_->update_free_space(candidate_pid, slotted.contiguous_free_space());
                            return RecordId{candidate_pid, insert_res.value()};
                        }
                    } else {
                        // Attempt compaction if fragmentation prevents insertion
                        slotted.compact();
                        if (slotted.can_insert(data.size())) {
                            auto insert_res = slotted.insert_record(data);
                            if (insert_res.is_ok()) {
                                guard.mark_dirty();
                                fsm_->update_free_space(candidate_pid, slotted.contiguous_free_space());
                                return RecordId{candidate_pid, insert_res.value()};
                            }
                        }
                        fsm_->update_free_space(candidate_pid, slotted.contiguous_free_space());
                    }
                }
            }
        }
    }

    // 2. Fallback path (when FSM is not present or didn't find candidate):
    // If no FSM, scan existing record pages (Page 1 .. total_pages - 1)
    if (fsm_ == nullptr) {
        const uint32_t total_pages = bpm_.page_file().page_count();
        for (page_id_t pid = 1; pid < total_pages; ++pid) {
            auto guard_res = bpm_.fetch_page_guard(pid);
            if (guard_res.is_err()) {
                continue;
            }

            auto& guard = guard_res.value();
            if (guard->page_type() == PageType::Record && SlottedPage::is_slotted_page(*guard)) {
                SlottedPage slotted(*guard);
                if (slotted.can_insert(data.size())) {
                    auto insert_res = slotted.insert_record(data);
                    if (insert_res.is_ok()) {
                        guard.mark_dirty();
                        return RecordId{pid, insert_res.value()};
                    }
                }
            }
        }
    }

    // 3. Allocate a new record page
    page_id_t new_pid = INVALID_PAGE_ID;
    auto new_guard_res = bpm_.new_page_guard(new_pid);
    if (new_guard_res.is_err()) {
        return new_guard_res.status();
    }

    auto& guard = new_guard_res.value();
    Status init_status = SlottedPage::initialize(*guard);
    if (!init_status.is_ok()) {
        return init_status;
    }

    SlottedPage slotted(*guard);
    auto insert_res = slotted.insert_record(data);
    if (insert_res.is_err()) {
        return insert_res.status();
    }

    guard.mark_dirty();
    if (fsm_ != nullptr) {
        fsm_->update_free_space(new_pid, slotted.contiguous_free_space());
    }

    return RecordId{new_pid, insert_res.value()};
}

Result<RecordId> RecordManager::insert_record(const Record& record) {
    std::vector<uint8_t> serialized = record.serialize();
    return insert_record(serialized);
}

Result<std::vector<uint8_t>> RecordManager::get_record(RecordId rid) {
    if (!rid.is_valid()) {
        return Status::invalid_argument("Invalid RecordId: " + rid.to_string());
    }
    if (rid.page_id == 0) {
        return Status::invalid_page_id("Cannot retrieve records from Page 0 (system metadata)");
    }

    auto guard_res = bpm_.fetch_page_guard(rid.page_id);
    if (guard_res.is_err()) {
        return guard_res.status();
    }

    auto& guard = guard_res.value();
    if (guard->page_type() != PageType::Record || !SlottedPage::is_slotted_page(*guard)) {
        return Status::page_corrupted("Page " + std::to_string(rid.page_id) + " is not a valid slotted record page");
    }

    SlottedPage slotted(*guard);
    auto rec_res = slotted.get_record(rid.slot_id);
    if (rec_res.is_err()) {
        return rec_res.status();
    }

    auto span = rec_res.value();
    return std::vector<uint8_t>(span.begin(), span.end());
}

Result<Record> RecordManager::get_typed_record(RecordId rid) {
    auto bytes_res = get_record(rid);
    if (bytes_res.is_err()) {
        return bytes_res.status();
    }
    return Record::deserialize(bytes_res.value());
}

Status RecordManager::update_record(RecordId rid, std::span<const uint8_t> new_data) {
    if (!rid.is_valid()) {
        return Status::invalid_argument("Invalid RecordId: " + rid.to_string());
    }
    if (rid.page_id == 0) {
        return Status::invalid_page_id("Cannot update records on Page 0 (system metadata)");
    }

    auto guard_res = bpm_.fetch_page_guard(rid.page_id);
    if (guard_res.is_err()) {
        return guard_res.status();
    }

    auto& guard = guard_res.value();
    if (guard->page_type() != PageType::Record || !SlottedPage::is_slotted_page(*guard)) {
        return Status::page_corrupted("Page " + std::to_string(rid.page_id) + " is not a valid slotted record page");
    }

    SlottedPage slotted(*guard);
    Status update_status = slotted.update_record(rid.slot_id, new_data);
    if (update_status.is_ok()) {
        guard.mark_dirty();
        if (fsm_ != nullptr) {
            fsm_->update_free_space(rid.page_id, slotted.contiguous_free_space());
        }
    }
    return update_status;
}

Status RecordManager::delete_record(RecordId rid) {
    if (!rid.is_valid()) {
        return Status::invalid_argument("Invalid RecordId: " + rid.to_string());
    }
    if (rid.page_id == 0) {
        return Status::invalid_page_id("Cannot delete records on Page 0 (system metadata)");
    }

    auto guard_res = bpm_.fetch_page_guard(rid.page_id);
    if (guard_res.is_err()) {
        return guard_res.status();
    }

    auto& guard = guard_res.value();
    if (guard->page_type() != PageType::Record || !SlottedPage::is_slotted_page(*guard)) {
        return Status::page_corrupted("Page " + std::to_string(rid.page_id) + " is not a valid slotted record page");
    }

    SlottedPage slotted(*guard);
    Status delete_status = slotted.delete_record(rid.slot_id);
    if (delete_status.is_ok()) {
        guard.mark_dirty();
        if (fsm_ != nullptr) {
            fsm_->update_free_space(rid.page_id, slotted.contiguous_free_space());
        }
    }
    return delete_status;
}

} // namespace klyrdb
