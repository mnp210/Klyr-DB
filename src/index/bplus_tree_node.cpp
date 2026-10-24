#include "klyrdb/index/bplus_tree_node.hpp"
#include <cstring>
#include <array>

namespace klyrdb {

Status BPlusTreeNode::init_leaf(Page& page, page_id_t parent_id) noexcept {
    page.set_page_type(PageType::BTreeLeaf);
    auto p = page.mutable_payload();
    std::memset(p.data(), 0, MAX_PAYLOAD_SIZE);

    encode_u16_be(static_cast<uint16_t>(PageType::BTreeLeaf), p.data() + OFFSET_BTREE_NODE_TYPE);
    encode_u16_be(0, p.data() + OFFSET_BTREE_ENTRY_COUNT);
    encode_u16_be(static_cast<uint16_t>(MAX_PAYLOAD_SIZE), p.data() + OFFSET_BTREE_FSP);
    encode_u16_be(0, p.data() + OFFSET_BTREE_FLAGS);
    encode_u32_be(parent_id, p.data() + OFFSET_BTREE_PARENT_PID);
    encode_u32_be(INVALID_PAGE_ID, p.data() + OFFSET_BTREE_NEXT_OR_P0);
    encode_u32_be(INVALID_PAGE_ID, p.data() + OFFSET_BTREE_PREV_PID);
    encode_u32_be(0, p.data() + OFFSET_BTREE_RESERVED);

    page.set_payload_size(static_cast<uint16_t>(MAX_PAYLOAD_SIZE));
    return Status::ok();
}

Status BPlusTreeNode::init_internal(Page& page, page_id_t first_child_id, page_id_t parent_id) noexcept {
    page.set_page_type(PageType::BTreeInternal);
    auto p = page.mutable_payload();
    std::memset(p.data(), 0, MAX_PAYLOAD_SIZE);

    encode_u16_be(static_cast<uint16_t>(PageType::BTreeInternal), p.data() + OFFSET_BTREE_NODE_TYPE);
    encode_u16_be(0, p.data() + OFFSET_BTREE_ENTRY_COUNT);
    encode_u16_be(static_cast<uint16_t>(MAX_PAYLOAD_SIZE), p.data() + OFFSET_BTREE_FSP);
    encode_u16_be(0, p.data() + OFFSET_BTREE_FLAGS);
    encode_u32_be(parent_id, p.data() + OFFSET_BTREE_PARENT_PID);
    encode_u32_be(first_child_id, p.data() + OFFSET_BTREE_NEXT_OR_P0);
    encode_u32_be(INVALID_PAGE_ID, p.data() + OFFSET_BTREE_PREV_PID);
    encode_u32_be(0, p.data() + OFFSET_BTREE_RESERVED);

    page.set_payload_size(static_cast<uint16_t>(MAX_PAYLOAD_SIZE));
    return Status::ok();
}

uint8_t* BPlusTreeNode::payload() noexcept {
    return page_.mutable_payload().data();
}

const uint8_t* BPlusTreeNode::payload() const noexcept {
    return page_.payload().data();
}

bool BPlusTreeNode::is_leaf() const noexcept {
    return decode_u16_be(payload() + OFFSET_BTREE_NODE_TYPE) == static_cast<uint16_t>(PageType::BTreeLeaf);
}

bool BPlusTreeNode::is_internal() const noexcept {
    return decode_u16_be(payload() + OFFSET_BTREE_NODE_TYPE) == static_cast<uint16_t>(PageType::BTreeInternal);
}

bool BPlusTreeNode::is_root() const noexcept {
    return (decode_u16_be(payload() + OFFSET_BTREE_FLAGS) & BTREE_FLAG_ROOT) != 0;
}

void BPlusTreeNode::set_is_root(bool root) noexcept {
    uint16_t flags = decode_u16_be(payload() + OFFSET_BTREE_FLAGS);
    if (root) {
        flags |= BTREE_FLAG_ROOT;
    } else {
        flags &= ~BTREE_FLAG_ROOT;
    }
    encode_u16_be(flags, payload() + OFFSET_BTREE_FLAGS);
}

uint16_t BPlusTreeNode::entry_count() const noexcept {
    return decode_u16_be(payload() + OFFSET_BTREE_ENTRY_COUNT);
}

void BPlusTreeNode::set_entry_count(uint16_t count) noexcept {
    encode_u16_be(count, payload() + OFFSET_BTREE_ENTRY_COUNT);
}

uint16_t BPlusTreeNode::get_fsp() const noexcept {
    return decode_u16_be(payload() + OFFSET_BTREE_FSP);
}

void BPlusTreeNode::set_fsp(uint16_t fsp) noexcept {
    encode_u16_be(fsp, payload() + OFFSET_BTREE_FSP);
}

page_id_t BPlusTreeNode::parent_page_id() const noexcept {
    return decode_u32_be(payload() + OFFSET_BTREE_PARENT_PID);
}

void BPlusTreeNode::set_parent_page_id(page_id_t pid) noexcept {
    encode_u32_be(pid, payload() + OFFSET_BTREE_PARENT_PID);
}

page_id_t BPlusTreeNode::next_page_id() const noexcept {
    return decode_u32_be(payload() + OFFSET_BTREE_NEXT_OR_P0);
}

void BPlusTreeNode::set_next_page_id(page_id_t pid) noexcept {
    encode_u32_be(pid, payload() + OFFSET_BTREE_NEXT_OR_P0);
}

page_id_t BPlusTreeNode::prev_page_id() const noexcept {
    return decode_u32_be(payload() + OFFSET_BTREE_PREV_PID);
}

void BPlusTreeNode::set_prev_page_id(page_id_t pid) noexcept {
    encode_u32_be(pid, payload() + OFFSET_BTREE_PREV_PID);
}

page_id_t BPlusTreeNode::first_child_id() const noexcept {
    return decode_u32_be(payload() + OFFSET_BTREE_NEXT_OR_P0);
}

void BPlusTreeNode::set_first_child_id(page_id_t pid) noexcept {
    encode_u32_be(pid, payload() + OFFSET_BTREE_NEXT_OR_P0);
}

uint16_t BPlusTreeNode::get_offset(uint16_t index) const noexcept {
    const size_t off = BTREE_HEADER_SIZE + (static_cast<size_t>(index) * 2);
    return decode_u16_be(payload() + off);
}

void BPlusTreeNode::set_offset(uint16_t index, uint16_t offset) noexcept {
    const size_t off = BTREE_HEADER_SIZE + (static_cast<size_t>(index) * 2);
    encode_u16_be(offset, payload() + off);
}

size_t BPlusTreeNode::free_space() const noexcept {
    const uint16_t fsp = get_fsp();
    const size_t table_end = BTREE_HEADER_SIZE + (static_cast<size_t>(entry_count()) * 2);
    if (fsp >= table_end) {
        return fsp - table_end;
    }
    return 0;
}

bool BPlusTreeNode::can_fit_leaf_entry(const IndexKey& key) const noexcept {
    const size_t needed = key.serialized_size() + 6 + 2; // key + RID (6) + offset (2)
    return free_space() >= needed;
}

bool BPlusTreeNode::can_fit_internal_entry(const IndexKey& key) const noexcept {
    const size_t needed = key.serialized_size() + 4 + 2; // key + child_id (4) + offset (2)
    return free_space() >= needed;
}

// --- Leaf Node Entry Operations ---

Result<IndexEntry> BPlusTreeNode::get_leaf_entry(uint16_t index) const {
    if (index >= entry_count()) {
        return Status::invalid_argument("Leaf entry index out of bounds");
    }
    const uint16_t off = get_offset(index);
    if (off < get_fsp() || off + 7 > MAX_PAYLOAD_SIZE) {
        return Status::index_corrupted("Corrupted leaf entry offset");
    }

    const uint8_t* p = payload() + off;
    const size_t rem = MAX_PAYLOAD_SIZE - off;
    auto key_res = IndexKey::decode(p, rem);
    if (key_res.is_err()) {
        return key_res.status();
    }

    const IndexKey key = key_res.value();
    const size_t key_len = key.serialized_size();
    if (rem < key_len + 6) {
        return Status::truncated_file("Buffer too small for RecordId in leaf entry");
    }

    const uint8_t* rid_p = p + key_len;
    RecordId rid;
    rid.page_id = decode_u32_be(rid_p);
    rid.slot_id = decode_u16_be(rid_p + 4);

    return IndexEntry(std::move(key), rid);
}

Result<IndexKey> BPlusTreeNode::get_leaf_key(uint16_t index) const {
    if (index >= entry_count()) {
        return Status::invalid_argument("Leaf entry index out of bounds");
    }
    const uint16_t off = get_offset(index);
    return IndexKey::decode(payload() + off, MAX_PAYLOAD_SIZE - off);
}

Result<RecordId> BPlusTreeNode::get_leaf_rid(uint16_t index) const {
    auto entry_res = get_leaf_entry(index);
    if (entry_res.is_err()) {
        return entry_res.status();
    }
    return entry_res.value().rid;
}

uint16_t BPlusTreeNode::find_leaf_entry_index(const IndexKey& key, const RecordId& rid) const {
    const uint16_t count = entry_count();
    uint16_t low = 0;
    uint16_t high = count;
    IndexEntry target(key, rid);

    while (low < high) {
        uint16_t mid = low + (high - low) / 2;
        auto cur_res = get_leaf_entry(mid);
        if (cur_res.is_err() || cur_res.value() < target) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low;
}

Status BPlusTreeNode::insert_leaf_entry(const IndexKey& key, const RecordId& rid) {
    const size_t entry_size = key.serialized_size() + 6;
    const size_t total_needed = entry_size + 2;

    if (free_space() < total_needed) {
        return Status::no_free_space("Leaf node lacks capacity for new entry");
    }

    const uint16_t target_idx = find_leaf_entry_index(key, rid);
    const uint16_t count = entry_count();

    const uint16_t new_fsp = static_cast<uint16_t>(get_fsp() - entry_size);
    uint8_t* dest = payload() + new_fsp;

    // Encode key followed by RecordId
    key.encode(dest);
    encode_u32_be(rid.page_id, dest + key.serialized_size());
    encode_u16_be(rid.slot_id, dest + key.serialized_size() + 4);

    // Shift offset table to make space for target_idx
    for (uint16_t i = count; i > target_idx; --i) {
        set_offset(i, get_offset(i - 1));
    }

    set_offset(target_idx, new_fsp);
    set_fsp(new_fsp);
    set_entry_count(count + 1);

    return Status::ok();
}

Status BPlusTreeNode::remove_leaf_entry(uint16_t index) {
    const uint16_t count = entry_count();
    if (index >= count) {
        return Status::invalid_argument("Index out of bounds for leaf entry deletion");
    }

    for (uint16_t i = index; i + 1 < count; ++i) {
        set_offset(i, get_offset(i + 1));
    }

    set_entry_count(count - 1);
    return Status::ok();
}

// --- Internal Node Entry Operations ---

Result<IndexKey> BPlusTreeNode::get_internal_key(uint16_t index) const {
    if (index >= entry_count()) {
        return Status::invalid_argument("Internal key index out of bounds");
    }
    const uint16_t off = get_offset(index);
    return IndexKey::decode(payload() + off, MAX_PAYLOAD_SIZE - off);
}

Result<page_id_t> BPlusTreeNode::get_internal_child_id(uint16_t index) const {
    if (index >= entry_count()) {
        return Status::invalid_argument("Internal child index out of bounds");
    }
    const uint16_t off = get_offset(index);
    const auto key_res = IndexKey::decode(payload() + off, MAX_PAYLOAD_SIZE - off);
    if (key_res.is_err()) {
        return key_res.status();
    }
    const size_t key_len = key_res.value().serialized_size();
    return decode_u32_be(payload() + off + key_len);
}

Result<page_id_t> BPlusTreeNode::find_child_page_id(const IndexKey& key) const {
    const uint16_t count = entry_count();
    if (count == 0) {
        return first_child_id();
    }

    // Binary search for first key > search_key
    uint16_t low = 0;
    uint16_t high = count;
    while (low < high) {
        uint16_t mid = low + (high - low) / 2;
        auto mid_key_res = get_internal_key(mid);
        if (mid_key_res.is_err()) {
            return mid_key_res.status();
        }
        if (mid_key_res.value() <= key) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }

    if (low == 0) {
        return first_child_id();
    }
    return get_internal_child_id(low - 1);
}

Status BPlusTreeNode::insert_internal_entry(const IndexKey& key, page_id_t right_child_id) {
    const size_t entry_size = key.serialized_size() + 4; // key + page_id (4)
    const size_t total_needed = entry_size + 2;

    if (free_space() < total_needed) {
        return Status::no_free_space("Internal node lacks capacity for new entry");
    }

    const uint16_t count = entry_count();

    // Binary search for insertion position
    uint16_t low = 0;
    uint16_t high = count;
    while (low < high) {
        uint16_t mid = low + (high - low) / 2;
        auto mid_key = get_internal_key(mid);
        if (mid_key.is_err() || mid_key.value() < key) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    const uint16_t target_idx = low;

    const uint16_t new_fsp = static_cast<uint16_t>(get_fsp() - entry_size);
    uint8_t* dest = payload() + new_fsp;

    key.encode(dest);
    encode_u32_be(right_child_id, dest + key.serialized_size());

    // Shift offset table
    for (uint16_t i = count; i > target_idx; --i) {
        set_offset(i, get_offset(i - 1));
    }

    set_offset(target_idx, new_fsp);
    set_fsp(new_fsp);
    set_entry_count(count + 1);

    return Status::ok();
}

Status BPlusTreeNode::remove_internal_entry(uint16_t index) {
    const uint16_t count = entry_count();
    if (index >= count) {
        return Status::invalid_argument("Index out of bounds for internal entry deletion");
    }

    for (uint16_t i = index; i + 1 < count; ++i) {
        set_offset(i, get_offset(i + 1));
    }

    set_entry_count(count - 1);
    return Status::ok();
}

Status BPlusTreeNode::compact() {
    const uint16_t count = entry_count();
    std::array<uint8_t, MAX_PAYLOAD_SIZE> scratch{};

    // Copy 24-byte header
    std::memcpy(scratch.data(), payload(), BTREE_HEADER_SIZE);

    uint16_t new_fsp = static_cast<uint16_t>(MAX_PAYLOAD_SIZE);

    for (uint16_t i = 0; i < count; ++i) {
        const uint16_t old_off = get_offset(i);
        size_t entry_size = 0;

        if (is_leaf()) {
            auto key_res = IndexKey::decode(payload() + old_off, MAX_PAYLOAD_SIZE - old_off);
            if (!key_res.is_ok()) return key_res.status();
            entry_size = key_res.value().serialized_size() + 6;
        } else {
            auto key_res = IndexKey::decode(payload() + old_off, MAX_PAYLOAD_SIZE - old_off);
            if (!key_res.is_ok()) return key_res.status();
            entry_size = key_res.value().serialized_size() + 4;
        }

        new_fsp = static_cast<uint16_t>(new_fsp - entry_size);
        std::memcpy(scratch.data() + new_fsp, payload() + old_off, entry_size);

        // Update offset in scratch table
        const size_t off_in_scratch = BTREE_HEADER_SIZE + (static_cast<size_t>(i) * 2);
        encode_u16_be(new_fsp, scratch.data() + off_in_scratch);
    }

    encode_u16_be(new_fsp, scratch.data() + OFFSET_BTREE_FSP);
    std::memcpy(payload(), scratch.data(), MAX_PAYLOAD_SIZE);

    return Status::ok();
}

} // namespace klyrdb
