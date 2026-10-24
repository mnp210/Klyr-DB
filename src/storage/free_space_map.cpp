#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/common/endian.hpp"
#include "klyrdb/storage/page_constants.hpp"
#include "klyrdb/storage/slotted_page.hpp"
#include <cstring>
#include <algorithm>

namespace klyrdb {

namespace {

constexpr size_t OFFSET_FSM_TYPE       = 0;  // 2 bytes: PageType::FreeSpaceMap (0x0007)
constexpr size_t OFFSET_FSM_NEXT_PAGE  = 2;  // 4 bytes: page_id_t
constexpr size_t OFFSET_FSM_BASE_PAGE  = 6;  // 4 bytes: page_id_t
constexpr size_t OFFSET_FSM_PAGE_COUNT = 10; // 2 bytes: uint16_t
constexpr size_t OFFSET_FSM_ENTRIES    = 12; // array of uint16_t free bytes

constexpr size_t MAX_FSM_ENTRIES_PER_PAGE = (MAX_PAYLOAD_SIZE - OFFSET_FSM_ENTRIES) / sizeof(uint16_t);

} // namespace

Result<std::unique_ptr<FreeSpaceMap>> FreeSpaceMap::open_or_create(BufferPoolManager& bpm) {
    page_id_t fsm_pid = bpm.page_file().metadata().fsm_page_id;

    if (fsm_pid == INVALID_PAGE_ID) {
        auto guard_res = bpm.new_page_guard(fsm_pid);
        if (guard_res.is_err()) {
            return guard_res.status();
        }

        auto& guard = guard_res.value();
        guard->set_page_type(PageType::FreeSpaceMap);
        auto p = guard->mutable_payload();
        std::memset(p.data(), 0, MAX_PAYLOAD_SIZE);

        encode_u16_be(static_cast<uint16_t>(PageType::FreeSpaceMap), p.data() + OFFSET_FSM_TYPE);
        encode_u32_be(INVALID_PAGE_ID, p.data() + OFFSET_FSM_NEXT_PAGE);
        encode_u32_be(0, p.data() + OFFSET_FSM_BASE_PAGE);
        encode_u16_be(0, p.data() + OFFSET_FSM_PAGE_COUNT);
        guard->set_payload_size(static_cast<uint16_t>(MAX_PAYLOAD_SIZE));
        guard.mark_dirty();

        auto meta = bpm.page_file().metadata();
        meta.fsm_page_id = fsm_pid;
        Status update_st = bpm.page_file().update_metadata(meta);
        if (!update_st.is_ok()) {
            return update_st;
        }
    } else {
        auto guard_res = bpm.fetch_page_guard(fsm_pid);
        if (guard_res.is_err()) {
            return guard_res.status();
        }
        if (guard_res.value()->page_type() != PageType::FreeSpaceMap) {
            return Status::page_corrupted("Page " + std::to_string(fsm_pid) + " is not a FreeSpaceMap page");
        }
    }

    return std::make_unique<FreeSpaceMap>(bpm, fsm_pid);
}

Result<page_id_t> FreeSpaceMap::find_page_with_space(size_t required_bytes) {
    page_id_t curr_fsm = root_page_id_;

    while (curr_fsm != INVALID_PAGE_ID) {
        auto guard_res = bpm_.fetch_page_guard(curr_fsm);
        if (guard_res.is_err()) {
            return guard_res.status();
        }

        auto& guard = guard_res.value();
        const uint8_t* p = guard->payload().data();

        const page_id_t base_page = decode_u32_be(p + OFFSET_FSM_BASE_PAGE);
        const uint16_t count = decode_u16_be(p + OFFSET_FSM_PAGE_COUNT);

        for (uint16_t i = 0; i < count; ++i) {
            const page_id_t candidate_pid = base_page + i;
            if (candidate_pid == 0 || candidate_pid == root_page_id_ ||
                candidate_pid == bpm_.page_file().metadata().catalog_page_id) {
                continue;
            }

            const uint16_t free_sp = decode_u16_be(p + OFFSET_FSM_ENTRIES + (i * 2));
            if (free_sp >= required_bytes) {
                return candidate_pid;
            }
        }

        curr_fsm = decode_u32_be(p + OFFSET_FSM_NEXT_PAGE);
    }

    return Status::no_free_space("No page with at least " + std::to_string(required_bytes) + " bytes found in FSM");
}

Status FreeSpaceMap::update_free_space(page_id_t pid, size_t free_bytes) {
    const uint16_t clamped_bytes = static_cast<uint16_t>(std::min<size_t>(free_bytes, MAX_PAYLOAD_SIZE));
    page_id_t curr_fsm = root_page_id_;

    while (curr_fsm != INVALID_PAGE_ID) {
        auto guard_res = bpm_.fetch_page_guard(curr_fsm);
        if (guard_res.is_err()) {
            return guard_res.status();
        }

        auto& guard = guard_res.value();
        uint8_t* p = guard->mutable_payload().data();

        const page_id_t base_page = decode_u32_be(p + OFFSET_FSM_BASE_PAGE);
        const page_id_t end_page = base_page + static_cast<page_id_t>(MAX_FSM_ENTRIES_PER_PAGE);

        if (pid >= base_page && pid < end_page) {
            const uint16_t idx = static_cast<uint16_t>(pid - base_page);
            uint16_t count = decode_u16_be(p + OFFSET_FSM_PAGE_COUNT);

            if (idx >= count) {
                // Zero-fill gaps between old count and idx
                for (uint16_t j = count; j < idx; ++j) {
                    encode_u16_be(0, p + OFFSET_FSM_ENTRIES + (j * 2));
                }
                count = idx + 1;
                encode_u16_be(count, p + OFFSET_FSM_PAGE_COUNT);
            }

            encode_u16_be(clamped_bytes, p + OFFSET_FSM_ENTRIES + (idx * 2));
            guard.mark_dirty();
            return Status::ok();
        }

        page_id_t next_fsm = decode_u32_be(p + OFFSET_FSM_NEXT_PAGE);
        if (next_fsm == INVALID_PAGE_ID) {
            // Allocate next FSM page
            page_id_t new_fsm_pid = INVALID_PAGE_ID;
            auto new_guard_res = bpm_.new_page_guard(new_fsm_pid);
            if (new_guard_res.is_err()) {
                return new_guard_res.status();
            }

            auto& new_guard = new_guard_res.value();
            new_guard->set_page_type(PageType::FreeSpaceMap);
            auto new_p = new_guard->mutable_payload();
            std::memset(new_p.data(), 0, MAX_PAYLOAD_SIZE);

            encode_u16_be(static_cast<uint16_t>(PageType::FreeSpaceMap), new_p.data() + OFFSET_FSM_TYPE);
            encode_u32_be(INVALID_PAGE_ID, new_p.data() + OFFSET_FSM_NEXT_PAGE);
            encode_u32_be(end_page, new_p.data() + OFFSET_FSM_BASE_PAGE);
            encode_u16_be(0, new_p.data() + OFFSET_FSM_PAGE_COUNT);
            new_guard->set_payload_size(static_cast<uint16_t>(MAX_PAYLOAD_SIZE));
            new_guard.mark_dirty();

            encode_u32_be(new_fsm_pid, p + OFFSET_FSM_NEXT_PAGE);
            guard.mark_dirty();

            curr_fsm = new_fsm_pid;
        } else {
            curr_fsm = next_fsm;
        }
    }

    return Status::io_error("Failed to update FSM for page " + std::to_string(pid));
}

Result<size_t> FreeSpaceMap::get_free_space(page_id_t pid) {
    page_id_t curr_fsm = root_page_id_;

    while (curr_fsm != INVALID_PAGE_ID) {
        auto guard_res = bpm_.fetch_page_guard(curr_fsm);
        if (guard_res.is_err()) {
            return guard_res.status();
        }

        auto& guard = guard_res.value();
        const uint8_t* p = guard->payload().data();

        const page_id_t base_page = decode_u32_be(p + OFFSET_FSM_BASE_PAGE);
        const uint16_t count = decode_u16_be(p + OFFSET_FSM_PAGE_COUNT);

        if (pid >= base_page && pid < base_page + count) {
            const uint16_t idx = static_cast<uint16_t>(pid - base_page);
            return static_cast<size_t>(decode_u16_be(p + OFFSET_FSM_ENTRIES + (idx * 2)));
        }

        curr_fsm = decode_u32_be(p + OFFSET_FSM_NEXT_PAGE);
    }

    return 0;
}

Status FreeSpaceMap::rebuild() {
    const uint32_t total_pages = bpm_.page_file().page_count();

    for (page_id_t pid = 1; pid < total_pages; ++pid) {
        if (pid == root_page_id_ || pid == bpm_.page_file().metadata().catalog_page_id) {
            update_free_space(pid, 0);
            continue;
        }

        auto guard_res = bpm_.fetch_page_guard(pid);
        if (guard_res.is_err()) {
            update_free_space(pid, 0);
            continue;
        }

        auto& guard = guard_res.value();
        if (guard->page_type() == PageType::Record && SlottedPage::is_slotted_page(*guard)) {
            SlottedPage slotted(*guard);
            update_free_space(pid, slotted.contiguous_free_space());
        } else {
            update_free_space(pid, 0);
        }
    }

    return Status::ok();
}

} // namespace klyrdb
