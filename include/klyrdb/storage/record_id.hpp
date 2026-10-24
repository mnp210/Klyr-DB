#pragma once

#include "klyrdb/common/types.hpp"
#include <string>
#include <functional>
#include <compare>

namespace klyrdb {

/// Persistent identifier locating a record by its page and slot index.
struct RecordId {
    page_id_t page_id{INVALID_PAGE_ID};
    slot_id_t slot_id{INVALID_SLOT_ID};

    constexpr RecordId() noexcept = default;
    constexpr RecordId(page_id_t pid, slot_id_t sid) noexcept : page_id(pid), slot_id(sid) {}

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return page_id != INVALID_PAGE_ID && slot_id != INVALID_SLOT_ID;
    }

    constexpr bool operator==(const RecordId& other) const noexcept = default;
    constexpr bool operator!=(const RecordId& other) const noexcept = default;

    constexpr auto operator<=>(const RecordId& other) const noexcept {
        if (auto cmp = page_id <=> other.page_id; cmp != 0) {
            return cmp;
        }
        return slot_id <=> other.slot_id;
    }

    [[nodiscard]] std::string to_string() const {
        return "[" + std::to_string(page_id) + ":" + std::to_string(slot_id) + "]";
    }
};

template <typename OStream>
inline OStream& operator<<(OStream& os, const RecordId& rid) {
    return os << rid.to_string();
}

} // namespace klyrdb

namespace std {
template <>
struct hash<klyrdb::RecordId> {
    size_t operator()(const klyrdb::RecordId& rid) const noexcept {
        return (static_cast<size_t>(rid.page_id) << 16) ^ static_cast<size_t>(rid.slot_id);
    }
};
} // namespace std
