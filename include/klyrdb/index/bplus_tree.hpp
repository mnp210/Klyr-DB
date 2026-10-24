#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_id.hpp"
#include "klyrdb/index/key.hpp"
#include "klyrdb/index/bplus_tree_node.hpp"
#include <functional>
#include <optional>
#include <vector>

namespace klyrdb {

/// Configuration parameters for B+ Tree node capacities.
struct BTreeConfig {
    size_t max_leaf_entries{64};
    size_t max_internal_entries{64};
};

/// Forward iterator for sequential leaf traversal in a B+ Tree.
class BPlusTreeIterator {
public:
    BPlusTreeIterator() noexcept = default;
    BPlusTreeIterator(BufferPoolManager* bpm, page_id_t leaf_page_id, uint16_t index) noexcept
        : bpm_(bpm), current_page_id_(leaf_page_id), current_index_(index) {
        load_current();
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return bpm_ != nullptr && current_page_id_ != INVALID_PAGE_ID;
    }

    [[nodiscard]] const IndexEntry& operator*() const noexcept {
        return current_entry_;
    }

    [[nodiscard]] const IndexEntry* operator->() const noexcept {
        return &current_entry_;
    }

    BPlusTreeIterator& operator++();

    bool operator==(const BPlusTreeIterator& other) const noexcept {
        if (!is_valid() && !other.is_valid()) return true;
        return bpm_ == other.bpm_ &&
               current_page_id_ == other.current_page_id_ &&
               current_index_ == other.current_index_;
    }

    bool operator!=(const BPlusTreeIterator& other) const noexcept {
        return !(*this == other);
    }

private:
    void load_current();

    BufferPoolManager* bpm_{nullptr};
    page_id_t current_page_id_{INVALID_PAGE_ID};
    uint16_t current_index_{0};
    IndexEntry current_entry_{};
};

/// Persistent, disk-backed B+ Tree index implementing point lookups,
/// duplicate-key resolution, range scans, dynamic node splits, balancing,
/// and underflow merges.
class BPlusTree {
public:
    using RootChangeCallback = std::function<void(page_id_t new_root_id)>;

    explicit BPlusTree(BufferPoolManager& bpm,
                       page_id_t root_page_id = INVALID_PAGE_ID,
                       BTreeConfig config = {}) noexcept
        : bpm_(bpm), root_page_id_(root_page_id), config_(config) {}

    [[nodiscard]] page_id_t root_page_id() const noexcept { return root_page_id_; }
    [[nodiscard]] bool is_empty() const noexcept { return root_page_id_ == INVALID_PAGE_ID; }
    [[nodiscard]] const BTreeConfig& config() const noexcept { return config_; }

    void set_on_root_change(RootChangeCallback cb) {
        on_root_change_ = std::move(cb);
    }

    /// Point lookup: returns the first matching RecordId, or KeyNotFound status.
    [[nodiscard]] Result<RecordId> find_one(const IndexKey& key);

    /// Point lookup for duplicate keys: returns all RecordIds matching key.
    [[nodiscard]] Result<std::vector<RecordId>> find(const IndexKey& key);

    /// Range scan: returns all IndexEntries whose keys fall within [low_key, high_key].
    [[nodiscard]] Result<std::vector<IndexEntry>> scan_range(
        const std::optional<IndexKey>& low_key = std::nullopt,
        const std::optional<IndexKey>& high_key = std::nullopt,
        bool inclusive_low = true,
        bool inclusive_high = true);

    /// Inserts (key, rid) into the tree, handling leaf splits, internal splits,
    /// and root promotions.
    Status insert(const IndexKey& key, const RecordId& rid);

    /// Deletes (key, rid) from the tree, handling leaf underflow, sibling redistribution,
    /// merging, and root collapses.
    Status delete_entry(const IndexKey& key, const RecordId& rid);

    /// Iterator beginning at the leftmost entry of the tree.
    [[nodiscard]] BPlusTreeIterator begin();

    /// Iterator representing past-the-end.
    [[nodiscard]] BPlusTreeIterator end() noexcept { return BPlusTreeIterator{}; }

    /// Iterator starting at the first entry >= key.
    [[nodiscard]] BPlusTreeIterator lower_bound(const IndexKey& key);

private:
    [[nodiscard]] Result<page_id_t> find_leaf_page(const IndexKey& key);
    [[nodiscard]] Result<page_id_t> find_leftmost_leaf_page();

    Status insert_into_leaf(page_id_t leaf_pid, const IndexKey& key, const RecordId& rid);
    Status split_leaf(page_id_t leaf_pid, const IndexKey& key, const RecordId& rid);
    Status insert_into_parent(page_id_t left_child_pid, const IndexKey& separator_key, page_id_t right_child_pid);
    Status split_internal(page_id_t internal_pid, const IndexKey& separator_key, page_id_t right_child_pid);

    Status handle_leaf_underflow(page_id_t leaf_pid);
    Status handle_internal_underflow(page_id_t internal_pid);

    void notify_root_change(page_id_t new_root_id);

    BufferPoolManager& bpm_;
    page_id_t root_page_id_{INVALID_PAGE_ID};
    BTreeConfig config_{};
    RootChangeCallback on_root_change_{nullptr};
};

} // namespace klyrdb
