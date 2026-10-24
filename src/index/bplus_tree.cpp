#include "klyrdb/index/bplus_tree.hpp"
#include <algorithm>

namespace klyrdb {

// --- BPlusTreeIterator Implementation ---

void BPlusTreeIterator::load_current() {
    if (bpm_ == nullptr || current_page_id_ == INVALID_PAGE_ID) {
        current_page_id_ = INVALID_PAGE_ID;
        return;
    }

    auto guard_res = bpm_->fetch_page_guard(current_page_id_);
    if (guard_res.is_err()) {
        current_page_id_ = INVALID_PAGE_ID;
        return;
    }

    auto& guard = guard_res.value();
    BPlusTreeNode node(*guard);
    if (!node.is_leaf()) {
        current_page_id_ = INVALID_PAGE_ID;
        return;
    }

    if (current_index_ >= node.entry_count()) {
        current_page_id_ = node.next_page_id();
        current_index_ = 0;
        if (current_page_id_ != INVALID_PAGE_ID) {
            load_current();
        }
        return;
    }

    auto entry_res = node.get_leaf_entry(current_index_);
    if (entry_res.is_ok()) {
        current_entry_ = entry_res.value();
    } else {
        current_page_id_ = INVALID_PAGE_ID;
    }
}

BPlusTreeIterator& BPlusTreeIterator::operator++() {
    if (!is_valid()) {
        return *this;
    }
    ++current_index_;
    load_current();
    return *this;
}

// --- BPlusTree Implementation ---

void BPlusTree::notify_root_change(page_id_t new_root_id) {
    if (on_root_change_) {
        on_root_change_(new_root_id);
    }
}

Result<page_id_t> BPlusTree::find_leaf_page(const IndexKey& key) {
    if (root_page_id_ == INVALID_PAGE_ID) {
        return Status::key_not_found("Index is empty");
    }

    page_id_t curr = root_page_id_;
    while (true) {
        auto guard_res = bpm_.fetch_page_guard(curr);
        if (guard_res.is_err()) {
            return guard_res.status();
        }

        auto& guard = guard_res.value();
        BPlusTreeNode node(*guard);
        if (node.is_leaf()) {
            return curr;
        }

        auto next_res = node.find_child_page_id(key);
        if (next_res.is_err()) {
            return next_res.status();
        }
        curr = next_res.value();
    }
}

Result<page_id_t> BPlusTree::find_leftmost_leaf_page() {
    if (root_page_id_ == INVALID_PAGE_ID) {
        return Status::key_not_found("Index is empty");
    }

    page_id_t curr = root_page_id_;
    while (true) {
        auto guard_res = bpm_.fetch_page_guard(curr);
        if (guard_res.is_err()) {
            return guard_res.status();
        }

        auto& guard = guard_res.value();
        BPlusTreeNode node(*guard);
        if (node.is_leaf()) {
            return curr;
        }
        curr = node.first_child_id();
    }
}

Result<RecordId> BPlusTree::find_one(const IndexKey& key) {
    auto leaf_res = find_leaf_page(key);
    if (leaf_res.is_err()) {
        return leaf_res.status();
    }

    auto guard_res = bpm_.fetch_page_guard(leaf_res.value());
    if (guard_res.is_err()) {
        return guard_res.status();
    }

    auto& guard = guard_res.value();
    BPlusTreeNode node(*guard);
    const uint16_t idx = node.find_leaf_entry_index(key, RecordId{0, 0});
    if (idx < node.entry_count()) {
        auto entry_res = node.get_leaf_entry(idx);
        if (entry_res.is_ok() && entry_res.value().key == key) {
            return entry_res.value().rid;
        }
    }

    return Status::key_not_found("Key " + key.to_string() + " not found in index");
}

Result<std::vector<RecordId>> BPlusTree::find(const IndexKey& key) {
    auto leaf_res = find_leaf_page(key);
    if (leaf_res.is_err()) {
        return leaf_res.status();
    }

    std::vector<RecordId> results;
    page_id_t curr_pid = leaf_res.value();
    uint16_t start_idx = 0;

    // Binary search for first candidate in initial leaf
    {
        auto guard_res = bpm_.fetch_page_guard(curr_pid);
        if (guard_res.is_err()) {
            return guard_res.status();
        }
        BPlusTreeNode node(*guard_res.value());
        start_idx = node.find_leaf_entry_index(key, RecordId{0, 0});
    }

    bool done = false;
    while (curr_pid != INVALID_PAGE_ID && !done) {
        auto guard_res = bpm_.fetch_page_guard(curr_pid);
        if (guard_res.is_err()) {
            return guard_res.status();
        }

        auto& guard = guard_res.value();
        BPlusTreeNode node(*guard);
        const uint16_t count = node.entry_count();

        for (uint16_t i = start_idx; i < count; ++i) {
            auto entry_res = node.get_leaf_entry(i);
            if (entry_res.is_err()) break;

            const auto& entry = entry_res.value();
            if (entry.key == key) {
                results.push_back(entry.rid);
            } else if (entry.key > key) {
                done = true;
                break;
            }
        }

        curr_pid = node.next_page_id();
        start_idx = 0;
    }

    return results;
}

Result<std::vector<IndexEntry>> BPlusTree::scan_range(
    const std::optional<IndexKey>& low_key,
    const std::optional<IndexKey>& high_key,
    bool inclusive_low,
    bool inclusive_high) {

    if (root_page_id_ == INVALID_PAGE_ID) {
        return std::vector<IndexEntry>{};
    }

    page_id_t curr_pid = INVALID_PAGE_ID;
    uint16_t start_idx = 0;

    if (low_key.has_value()) {
        auto leaf_res = find_leaf_page(low_key.value());
        if (leaf_res.is_err()) return leaf_res.status();
        curr_pid = leaf_res.value();

        auto guard_res = bpm_.fetch_page_guard(curr_pid);
        if (guard_res.is_err()) return guard_res.status();
        BPlusTreeNode node(*guard_res.value());

        start_idx = node.find_leaf_entry_index(low_key.value(), RecordId{0, 0});
        if (!inclusive_low) {
            while (start_idx < node.entry_count()) {
                auto k_res = node.get_leaf_key(start_idx);
                if (k_res.is_ok() && k_res.value() == low_key.value()) {
                    ++start_idx;
                } else {
                    break;
                }
            }
        }
    } else {
        auto leaf_res = find_leftmost_leaf_page();
        if (leaf_res.is_err()) return leaf_res.status();
        curr_pid = leaf_res.value();
        start_idx = 0;
    }

    std::vector<IndexEntry> results;
    bool done = false;

    while (curr_pid != INVALID_PAGE_ID && !done) {
        auto guard_res = bpm_.fetch_page_guard(curr_pid);
        if (guard_res.is_err()) return guard_res.status();

        auto& guard = guard_res.value();
        BPlusTreeNode node(*guard);
        const uint16_t count = node.entry_count();

        for (uint16_t i = start_idx; i < count; ++i) {
            auto entry_res = node.get_leaf_entry(i);
            if (entry_res.is_err()) break;

            const auto& entry = entry_res.value();
            if (high_key.has_value()) {
                if (inclusive_high) {
                    if (entry.key > high_key.value()) {
                        done = true;
                        break;
                    }
                } else {
                    if (entry.key >= high_key.value()) {
                        done = true;
                        break;
                    }
                }
            }
            results.push_back(entry);
        }

        curr_pid = node.next_page_id();
        start_idx = 0;
    }

    return results;
}

BPlusTreeIterator BPlusTree::begin() {
    if (root_page_id_ == INVALID_PAGE_ID) {
        return end();
    }
    auto leaf_res = find_leftmost_leaf_page();
    if (leaf_res.is_err()) {
        return end();
    }
    return BPlusTreeIterator(&bpm_, leaf_res.value(), 0);
}

BPlusTreeIterator BPlusTree::lower_bound(const IndexKey& key) {
    if (root_page_id_ == INVALID_PAGE_ID) {
        return end();
    }
    auto leaf_res = find_leaf_page(key);
    if (leaf_res.is_err()) {
        return end();
    }
    page_id_t leaf_pid = leaf_res.value();
    auto guard_res = bpm_.fetch_page_guard(leaf_pid);
    if (guard_res.is_err()) {
        return end();
    }
    BPlusTreeNode node(*guard_res.value());
    uint16_t idx = node.find_leaf_entry_index(key, RecordId{0, 0});
    return BPlusTreeIterator(&bpm_, leaf_pid, idx);
}

Status BPlusTree::insert(const IndexKey& key, const RecordId& rid) {
    if (root_page_id_ == INVALID_PAGE_ID) {
        page_id_t new_root_pid = INVALID_PAGE_ID;
        auto guard_res = bpm_.new_page_guard(new_root_pid);
        if (guard_res.is_err()) return guard_res.status();

        auto& guard = guard_res.value();
        Status init_st = BPlusTreeNode::init_leaf(*guard, INVALID_PAGE_ID);
        if (!init_st.is_ok()) return init_st;

        BPlusTreeNode node(*guard);
        node.set_is_root(true);
        Status ins_st = node.insert_leaf_entry(key, rid);
        if (!ins_st.is_ok()) return ins_st;

        guard.mark_dirty();
        root_page_id_ = new_root_pid;
        notify_root_change(root_page_id_);
        return Status::ok();
    }

    auto leaf_res = find_leaf_page(key);
    if (leaf_res.is_err()) return leaf_res.status();
    return insert_into_leaf(leaf_res.value(), key, rid);
}

Status BPlusTree::insert_into_leaf(page_id_t leaf_pid, const IndexKey& key, const RecordId& rid) {
    auto guard_res = bpm_.fetch_page_guard(leaf_pid);
    if (guard_res.is_err()) return guard_res.status();

    auto& guard = guard_res.value();
    BPlusTreeNode node(*guard);

    // Duplicate detection
    const uint16_t idx = node.find_leaf_entry_index(key, rid);
    if (idx < node.entry_count()) {
        auto existing = node.get_leaf_entry(idx);
        if (existing.is_ok() && existing.value().key == key && existing.value().rid == rid) {
            return Status::duplicate_key("Duplicate index entry (key, rid) already exists");
        }
    }

    if (node.entry_count() < config_.max_leaf_entries && node.can_fit_leaf_entry(key)) {
        Status st = node.insert_leaf_entry(key, rid);
        if (st.is_ok()) {
            guard.mark_dirty();
        }
        return st;
    }

    // Leaf overflow -> split
    return split_leaf(leaf_pid, key, rid);
}

Status BPlusTree::split_leaf(page_id_t leaf_pid, const IndexKey& key, const RecordId& rid) {
    bool is_root_split = false;
    IndexKey separator_key;
    page_id_t new_leaf_pid = INVALID_PAGE_ID;

    {
        auto leaf_guard_res = bpm_.fetch_page_guard(leaf_pid);
        if (leaf_guard_res.is_err()) return leaf_guard_res.status();

        auto& leaf_guard = leaf_guard_res.value();
        BPlusTreeNode leaf(*leaf_guard);

        // 1. Gather all existing entries + new entry
        std::vector<IndexEntry> entries;
        entries.reserve(leaf.entry_count() + 1);
        for (uint16_t i = 0; i < leaf.entry_count(); ++i) {
            entries.push_back(leaf.get_leaf_entry(i).value());
        }

        IndexEntry new_entry{key, rid};
        auto it = std::lower_bound(entries.begin(), entries.end(), new_entry);
        entries.insert(it, std::move(new_entry));

        // 2. Allocate new leaf page
        auto new_leaf_guard_res = bpm_.new_page_guard(new_leaf_pid);
        if (new_leaf_guard_res.is_err()) return new_leaf_guard_res.status();

        auto& new_leaf_guard = new_leaf_guard_res.value();
        Status init_st = BPlusTreeNode::init_leaf(*new_leaf_guard, leaf.parent_page_id());
        if (!init_st.is_ok()) return init_st;

        BPlusTreeNode new_leaf(*new_leaf_guard);

        // 3. Sibling link management
        const page_id_t old_next = leaf.next_page_id();
        new_leaf.set_next_page_id(old_next);
        new_leaf.set_prev_page_id(leaf_pid);
        leaf.set_next_page_id(new_leaf_pid);

        if (old_next != INVALID_PAGE_ID) {
            auto next_guard_res = bpm_.fetch_page_guard(old_next);
            if (next_guard_res.is_ok()) {
                BPlusTreeNode next_node(*next_guard_res.value());
                next_node.set_prev_page_id(new_leaf_pid);
                next_guard_res.value().mark_dirty();
            }
        }

        // 4. Distribute entries evenly
        const size_t mid = entries.size() / 2;

        is_root_split = leaf.is_root();
        const page_id_t parent_pid = leaf.parent_page_id();
        const page_id_t prev_pid = leaf.prev_page_id();

        // Re-initialize left leaf with preserved links
        BPlusTreeNode::init_leaf(*leaf_guard, parent_pid);
        leaf.set_is_root(is_root_split);
        leaf.set_prev_page_id(prev_pid);
        leaf.set_next_page_id(new_leaf_pid);

        for (size_t i = 0; i < mid; ++i) {
            leaf.insert_leaf_entry(entries[i].key, entries[i].rid);
        }
        for (size_t i = mid; i < entries.size(); ++i) {
            new_leaf.insert_leaf_entry(entries[i].key, entries[i].rid);
        }

        separator_key = entries[mid].key;

        if (is_root_split) {
            page_id_t new_root_pid = INVALID_PAGE_ID;
            auto new_root_guard_res = bpm_.new_page_guard(new_root_pid);
            if (new_root_guard_res.is_err()) return new_root_guard_res.status();

            auto& new_root_guard = new_root_guard_res.value();
            BPlusTreeNode::init_internal(*new_root_guard, leaf_pid, INVALID_PAGE_ID);
            BPlusTreeNode new_root(*new_root_guard);
            new_root.set_is_root(true);
            new_root.insert_internal_entry(separator_key, new_leaf_pid);

            leaf.set_is_root(false);
            leaf.set_parent_page_id(new_root_pid);
            new_leaf.set_parent_page_id(new_root_pid);

            leaf_guard.mark_dirty();
            new_leaf_guard.mark_dirty();
            new_root_guard.mark_dirty();

            root_page_id_ = new_root_pid;
            notify_root_change(root_page_id_);
            return Status::ok();
        }

        leaf_guard.mark_dirty();
        new_leaf_guard.mark_dirty();
    }

    return insert_into_parent(leaf_pid, separator_key, new_leaf_pid);
}

Status BPlusTree::insert_into_parent(page_id_t left_child_pid, const IndexKey& separator_key, page_id_t right_child_pid) {
    page_id_t parent_pid = INVALID_PAGE_ID;
    {
        auto left_guard_res = bpm_.fetch_page_guard(left_child_pid);
        if (left_guard_res.is_err()) return left_guard_res.status();
        BPlusTreeNode left_node(*left_guard_res.value());
        parent_pid = left_node.parent_page_id();
    }

    if (parent_pid == INVALID_PAGE_ID) {
        return Status::index_corrupted("Cannot insert separator into invalid parent");
    }

    auto parent_guard_res = bpm_.fetch_page_guard(parent_pid);
    if (parent_guard_res.is_err()) return parent_guard_res.status();

    auto& parent_guard = parent_guard_res.value();
    BPlusTreeNode parent(*parent_guard);

    if (parent.entry_count() < config_.max_internal_entries && parent.can_fit_internal_entry(separator_key)) {
        Status st = parent.insert_internal_entry(separator_key, right_child_pid);
        if (!st.is_ok()) return st;

        // Ensure right child's parent is correctly assigned
        auto right_guard_res = bpm_.fetch_page_guard(right_child_pid);
        if (right_guard_res.is_ok()) {
            BPlusTreeNode right_node(*right_guard_res.value());
            right_node.set_parent_page_id(parent_pid);
            right_guard_res.value().mark_dirty();
        }

        parent_guard.mark_dirty();
        return Status::ok();
    }

    // Internal node overflow -> split
    return split_internal(parent_pid, separator_key, right_child_pid);
}

Status BPlusTree::split_internal(page_id_t internal_pid, const IndexKey& separator_key, page_id_t right_child_pid) {
    bool is_root_split = false;
    IndexKey push_up_key;
    page_id_t new_internal_pid = INVALID_PAGE_ID;

    {
        auto internal_guard_res = bpm_.fetch_page_guard(internal_pid);
        if (internal_guard_res.is_err()) return internal_guard_res.status();

        auto& internal_guard = internal_guard_res.value();
        BPlusTreeNode internal(*internal_guard);

        struct InternalEntry {
            IndexKey key;
            page_id_t child_id;
        };

        std::vector<InternalEntry> entries;
        entries.reserve(internal.entry_count() + 1);
        for (uint16_t i = 0; i < internal.entry_count(); ++i) {
            entries.push_back({internal.get_internal_key(i).value(), internal.get_internal_child_id(i).value()});
        }

        InternalEntry new_entry{separator_key, right_child_pid};
        auto it = std::lower_bound(entries.begin(), entries.end(), new_entry,
            [](const InternalEntry& a, const InternalEntry& b) {
                return a.key < b.key;
            });
        entries.insert(it, std::move(new_entry));

        const size_t mid = entries.size() / 2;
        push_up_key = entries[mid].key;
        const page_id_t right_first_child = entries[mid].child_id;

        auto new_internal_guard_res = bpm_.new_page_guard(new_internal_pid);
        if (new_internal_guard_res.is_err()) return new_internal_guard_res.status();

        auto& new_internal_guard = new_internal_guard_res.value();
        Status init_st = BPlusTreeNode::init_internal(*new_internal_guard, right_first_child, internal.parent_page_id());
        if (!init_st.is_ok()) return init_st;

        BPlusTreeNode new_internal(*new_internal_guard);

        // Update parent of right_first_child
        {
            auto rfc_guard_res = bpm_.fetch_page_guard(right_first_child);
            if (rfc_guard_res.is_ok()) {
                BPlusTreeNode rfc_node(*rfc_guard_res.value());
                rfc_node.set_parent_page_id(new_internal_pid);
                rfc_guard_res.value().mark_dirty();
            }
        }

        is_root_split = internal.is_root();
        const page_id_t parent_pid = internal.parent_page_id();
        const page_id_t orig_first_child = internal.first_child_id();

        // Re-initialize left internal node
        BPlusTreeNode::init_internal(*internal_guard, orig_first_child, parent_pid);
        internal.set_is_root(is_root_split);

        for (size_t i = 0; i < mid; ++i) {
            internal.insert_internal_entry(entries[i].key, entries[i].child_id);
        }

        for (size_t i = mid + 1; i < entries.size(); ++i) {
            new_internal.insert_internal_entry(entries[i].key, entries[i].child_id);
            auto child_guard_res = bpm_.fetch_page_guard(entries[i].child_id);
            if (child_guard_res.is_ok()) {
                BPlusTreeNode child_node(*child_guard_res.value());
                child_node.set_parent_page_id(new_internal_pid);
                child_guard_res.value().mark_dirty();
            }
        }

        if (is_root_split) {
            page_id_t new_root_pid = INVALID_PAGE_ID;
            auto new_root_guard_res = bpm_.new_page_guard(new_root_pid);
            if (new_root_guard_res.is_err()) return new_root_guard_res.status();

            auto& new_root_guard = new_root_guard_res.value();
            BPlusTreeNode::init_internal(*new_root_guard, internal_pid, INVALID_PAGE_ID);
            BPlusTreeNode new_root(*new_root_guard);
            new_root.set_is_root(true);
            new_root.insert_internal_entry(push_up_key, new_internal_pid);

            internal.set_is_root(false);
            internal.set_parent_page_id(new_root_pid);
            new_internal.set_parent_page_id(new_root_pid);

            internal_guard.mark_dirty();
            new_internal_guard.mark_dirty();
            new_root_guard.mark_dirty();

            root_page_id_ = new_root_pid;
            notify_root_change(root_page_id_);
            return Status::ok();
        }

        internal_guard.mark_dirty();
        new_internal_guard.mark_dirty();
    }

    return insert_into_parent(internal_pid, push_up_key, new_internal_pid);
}

Status BPlusTree::delete_entry(const IndexKey& key, const RecordId& rid) {
    if (root_page_id_ == INVALID_PAGE_ID) {
        return Status::key_not_found("Index is empty");
    }

    auto leaf_res = find_leaf_page(key);
    if (leaf_res.is_err()) return leaf_res.status();

    page_id_t leaf_pid = leaf_res.value();
    auto leaf_guard_res = bpm_.fetch_page_guard(leaf_pid);
    if (leaf_guard_res.is_err()) return leaf_guard_res.status();

    auto& leaf_guard = leaf_guard_res.value();
    BPlusTreeNode leaf(*leaf_guard);

    const uint16_t idx = leaf.find_leaf_entry_index(key, rid);
    if (idx >= leaf.entry_count()) {
        return Status::key_not_found("Index entry not found");
    }

    auto existing = leaf.get_leaf_entry(idx);
    if (existing.is_err() || existing.value().key != key || existing.value().rid != rid) {
        return Status::key_not_found("Index entry not found");
    }

    Status rm_st = leaf.remove_leaf_entry(idx);
    if (!rm_st.is_ok()) return rm_st;
    leaf.compact();
    leaf_guard.mark_dirty();

    if (leaf.is_root()) {
        return Status::ok();
    }

    if (leaf.entry_count() < (config_.max_leaf_entries / 2)) {
        return handle_leaf_underflow(leaf_pid);
    }

    return Status::ok();
}

Status BPlusTree::handle_leaf_underflow(page_id_t leaf_pid) {
    page_id_t parent_pid = INVALID_PAGE_ID;
    {
        auto leaf_guard_res = bpm_.fetch_page_guard(leaf_pid);
        if (leaf_guard_res.is_err()) return leaf_guard_res.status();
        BPlusTreeNode leaf(*leaf_guard_res.value());
        parent_pid = leaf.parent_page_id();
    }

    if (parent_pid == INVALID_PAGE_ID) return Status::ok();

    auto parent_guard_res = bpm_.fetch_page_guard(parent_pid);
    if (parent_guard_res.is_err()) return parent_guard_res.status();

    auto& parent_guard = parent_guard_res.value();
    BPlusTreeNode parent(*parent_guard);

    int child_idx = -1;
    if (parent.first_child_id() == leaf_pid) {
        child_idx = 0;
    } else {
        for (uint16_t i = 0; i < parent.entry_count(); ++i) {
            if (parent.get_internal_child_id(i).value() == leaf_pid) {
                child_idx = static_cast<int>(i) + 1;
                break;
            }
        }
    }

    if (child_idx == -1) {
        return Status::index_corrupted("Leaf page not found in parent children");
    }

    // Try borrowing from left sibling
    if (child_idx > 0) {
        page_id_t left_sibling_pid = (child_idx == 1) ? parent.first_child_id()
                                                      : parent.get_internal_child_id(static_cast<uint16_t>(child_idx - 2)).value();
        auto left_guard_res = bpm_.fetch_page_guard(left_sibling_pid);
        if (left_guard_res.is_ok()) {
            auto& left_guard = left_guard_res.value();
            BPlusTreeNode left_sibling(*left_guard);
            if (left_sibling.entry_count() > (config_.max_leaf_entries / 2)) {
                // Borrow from left sibling
                auto last_entry = left_sibling.get_leaf_entry(left_sibling.entry_count() - 1).value();
                left_sibling.remove_leaf_entry(left_sibling.entry_count() - 1);
                left_sibling.compact();

                auto leaf_guard_res = bpm_.fetch_page_guard(leaf_pid);
                auto& leaf_guard = leaf_guard_res.value();
                BPlusTreeNode leaf(*leaf_guard);
                leaf.insert_leaf_entry(last_entry.key, last_entry.rid);

                // Update parent separator
                const uint16_t parent_entry_idx = static_cast<uint16_t>(child_idx - 1);
                page_id_t cid = parent.get_internal_child_id(parent_entry_idx).value();
                parent.remove_internal_entry(parent_entry_idx);
                parent.insert_internal_entry(leaf.get_leaf_key(0).value(), cid);

                left_guard.mark_dirty();
                leaf_guard.mark_dirty();
                parent_guard.mark_dirty();
                return Status::ok();
            }
        }
    }

    // Try borrowing from right sibling
    if (child_idx < static_cast<int>(parent.entry_count())) {
        const uint16_t right_idx = static_cast<uint16_t>(child_idx);
        page_id_t right_sibling_pid = parent.get_internal_child_id(right_idx).value();
        auto right_guard_res = bpm_.fetch_page_guard(right_sibling_pid);
        if (right_guard_res.is_ok()) {
            auto& right_guard = right_guard_res.value();
            BPlusTreeNode right_sibling(*right_guard);
            if (right_sibling.entry_count() > (config_.max_leaf_entries / 2)) {
                // Borrow from right sibling
                auto first_entry = right_sibling.get_leaf_entry(0).value();
                right_sibling.remove_leaf_entry(0);
                right_sibling.compact();

                auto leaf_guard_res = bpm_.fetch_page_guard(leaf_pid);
                auto& leaf_guard = leaf_guard_res.value();
                BPlusTreeNode leaf(*leaf_guard);
                leaf.insert_leaf_entry(first_entry.key, first_entry.rid);

                // Update parent separator
                page_id_t cid = parent.get_internal_child_id(right_idx).value();
                parent.remove_internal_entry(right_idx);
                parent.insert_internal_entry(right_sibling.get_leaf_key(0).value(), cid);

                right_guard.mark_dirty();
                leaf_guard.mark_dirty();
                parent_guard.mark_dirty();
                return Status::ok();
            }
        }
    }

    // If borrowing not possible: Merge with right sibling or left sibling
    if (child_idx < static_cast<int>(parent.entry_count())) {
        // Merge with right sibling
        const uint16_t right_idx = static_cast<uint16_t>(child_idx);
        page_id_t right_sibling_pid = parent.get_internal_child_id(right_idx).value();
        auto leaf_guard_res = bpm_.fetch_page_guard(leaf_pid);
        auto right_guard_res = bpm_.fetch_page_guard(right_sibling_pid);
        if (leaf_guard_res.is_err() || right_guard_res.is_err()) return Status::index_corrupted("Failed to fetch leaves for merge");

        auto& leaf_guard = leaf_guard_res.value();
        auto& right_guard = right_guard_res.value();
        BPlusTreeNode leaf(*leaf_guard);
        BPlusTreeNode right_sibling(*right_guard);

        for (uint16_t i = 0; i < right_sibling.entry_count(); ++i) {
            auto e = right_sibling.get_leaf_entry(i).value();
            leaf.insert_leaf_entry(e.key, e.rid);
        }

        const page_id_t next_pid = right_sibling.next_page_id();
        leaf.set_next_page_id(next_pid);
        if (next_pid != INVALID_PAGE_ID) {
            auto next_guard_res = bpm_.fetch_page_guard(next_pid);
            if (next_guard_res.is_ok()) {
                BPlusTreeNode next_node(*next_guard_res.value());
                next_node.set_prev_page_id(leaf_pid);
                next_guard_res.value().mark_dirty();
            }
        }

        parent.remove_internal_entry(right_idx);
        parent.compact();
        parent_guard.mark_dirty();
        leaf_guard.mark_dirty();

        bpm_.delete_page(right_sibling_pid);
        return handle_internal_underflow(parent_pid);
    } else if (child_idx > 0) {
        // Merge with left sibling
        page_id_t left_sibling_pid = (child_idx == 1) ? parent.first_child_id()
                                                      : parent.get_internal_child_id(static_cast<uint16_t>(child_idx - 2)).value();
        auto left_guard_res = bpm_.fetch_page_guard(left_sibling_pid);
        auto leaf_guard_res = bpm_.fetch_page_guard(leaf_pid);
        if (left_guard_res.is_err() || leaf_guard_res.is_err()) return Status::index_corrupted("Failed to fetch leaves for merge");

        auto& left_guard = left_guard_res.value();
        auto& leaf_guard = leaf_guard_res.value();
        BPlusTreeNode left_sibling(*left_guard);
        BPlusTreeNode leaf(*leaf_guard);

        for (uint16_t i = 0; i < leaf.entry_count(); ++i) {
            auto e = leaf.get_leaf_entry(i).value();
            left_sibling.insert_leaf_entry(e.key, e.rid);
        }

        const page_id_t next_pid = leaf.next_page_id();
        left_sibling.set_next_page_id(next_pid);
        if (next_pid != INVALID_PAGE_ID) {
            auto next_guard_res = bpm_.fetch_page_guard(next_pid);
            if (next_guard_res.is_ok()) {
                BPlusTreeNode next_node(*next_guard_res.value());
                next_node.set_prev_page_id(left_sibling_pid);
                next_guard_res.value().mark_dirty();
            }
        }

        const uint16_t left_sep_idx = static_cast<uint16_t>(child_idx - 1);
        parent.remove_internal_entry(left_sep_idx);
        parent.compact();
        parent_guard.mark_dirty();
        left_guard.mark_dirty();

        bpm_.delete_page(leaf_pid);
        return handle_internal_underflow(parent_pid);
    }

    return Status::ok();
}

Status BPlusTree::handle_internal_underflow(page_id_t internal_pid) {
    auto guard_res = bpm_.fetch_page_guard(internal_pid);
    if (guard_res.is_err()) return guard_res.status();

    auto& guard = guard_res.value();
    BPlusTreeNode node(*guard);

    if (node.is_root()) {
        if (node.entry_count() == 0) {
            // Root collapses into its only child
            page_id_t new_root_pid = node.first_child_id();
            auto new_root_guard_res = bpm_.fetch_page_guard(new_root_pid);
            if (new_root_guard_res.is_ok()) {
                auto& nr_guard = new_root_guard_res.value();
                BPlusTreeNode new_root(*nr_guard);
                new_root.set_is_root(true);
                new_root.set_parent_page_id(INVALID_PAGE_ID);
                nr_guard.mark_dirty();

                bpm_.delete_page(internal_pid);
                root_page_id_ = new_root_pid;
                notify_root_change(root_page_id_);
            }
        }
        return Status::ok();
    }

    // For non-root internal node underflow, we can merge with sibling if below threshold
    if (node.entry_count() >= (config_.max_internal_entries / 2)) {
        return Status::ok();
    }

    page_id_t parent_pid = node.parent_page_id();
    if (parent_pid == INVALID_PAGE_ID) return Status::ok();

    auto parent_guard_res = bpm_.fetch_page_guard(parent_pid);
    if (parent_guard_res.is_err()) return parent_guard_res.status();

    auto& parent_guard = parent_guard_res.value();
    BPlusTreeNode parent(*parent_guard);

    int child_idx = -1;
    if (parent.first_child_id() == internal_pid) {
        child_idx = 0;
    } else {
        for (uint16_t i = 0; i < parent.entry_count(); ++i) {
            if (parent.get_internal_child_id(i).value() == internal_pid) {
                child_idx = static_cast<int>(i) + 1;
                break;
            }
        }
    }

    if (child_idx == -1) return Status::ok();

    // Internal node merge with right sibling
    if (child_idx < static_cast<int>(parent.entry_count())) {
        const uint16_t right_idx = static_cast<uint16_t>(child_idx);
        page_id_t right_sibling_pid = parent.get_internal_child_id(right_idx).value();
        auto right_guard_res = bpm_.fetch_page_guard(right_sibling_pid);
        if (right_guard_res.is_ok()) {
            auto& right_guard = right_guard_res.value();
            BPlusTreeNode right_sibling(*right_guard);

            // Pull down parent separator key
            IndexKey parent_sep = parent.get_internal_key(right_idx).value();
            node.insert_internal_entry(parent_sep, right_sibling.first_child_id());

            // Reparent right_sibling first child
            auto rfc_guard = bpm_.fetch_page_guard(right_sibling.first_child_id());
            if (rfc_guard.is_ok()) {
                BPlusTreeNode rfc(*rfc_guard.value());
                rfc.set_parent_page_id(internal_pid);
                rfc_guard.value().mark_dirty();
            }

            // Move entries from right sibling to node
            for (uint16_t i = 0; i < right_sibling.entry_count(); ++i) {
                IndexKey k = right_sibling.get_internal_key(i).value();
                page_id_t cid = right_sibling.get_internal_child_id(i).value();
                node.insert_internal_entry(k, cid);

                auto c_guard = bpm_.fetch_page_guard(cid);
                if (c_guard.is_ok()) {
                    BPlusTreeNode c(*c_guard.value());
                    c.set_parent_page_id(internal_pid);
                    c_guard.value().mark_dirty();
                }
            }

            parent.remove_internal_entry(right_idx);
            parent.compact();
            parent_guard.mark_dirty();
            guard.mark_dirty();

            bpm_.delete_page(right_sibling_pid);
            return handle_internal_underflow(parent_pid);
        }
    }

    return Status::ok();
}

} // namespace klyrdb
