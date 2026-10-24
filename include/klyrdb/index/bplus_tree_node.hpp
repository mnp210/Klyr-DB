#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/storage/page_constants.hpp"
#include "klyrdb/storage/page.hpp"
#include "klyrdb/index/key.hpp"
#include <span>
#include <vector>

namespace klyrdb {

inline constexpr size_t BTREE_HEADER_SIZE = 24;

inline constexpr size_t OFFSET_BTREE_NODE_TYPE   = 0;  // 2 bytes: uint16_t (PageType::BTreeLeaf or BTreeInternal)
inline constexpr size_t OFFSET_BTREE_ENTRY_COUNT = 2;  // 2 bytes: uint16_t
inline constexpr size_t OFFSET_BTREE_FSP         = 4;  // 2 bytes: uint16_t (free space pointer)
inline constexpr size_t OFFSET_BTREE_FLAGS       = 6;  // 2 bytes: uint16_t
inline constexpr size_t OFFSET_BTREE_PARENT_PID  = 8;  // 4 bytes: uint32_t (page_id_t)
inline constexpr size_t OFFSET_BTREE_NEXT_OR_P0  = 12; // 4 bytes: uint32_t (Leaf: next_page_id, Internal: child_0)
inline constexpr size_t OFFSET_BTREE_PREV_PID    = 16; // 4 bytes: uint32_t (Leaf: prev_page_id, Internal: reserved)
inline constexpr size_t OFFSET_BTREE_RESERVED    = 20; // 4 bytes: uint32_t

inline constexpr uint16_t BTREE_FLAG_ROOT = 0x0001;

/// Represents an in-memory view and operator over a persistent B+ tree node page.
class BPlusTreeNode {
public:
    explicit BPlusTreeNode(Page& page) noexcept : page_(page) {}

    static Status init_leaf(Page& page, page_id_t parent_id = INVALID_PAGE_ID) noexcept;
    static Status init_internal(Page& page, page_id_t first_child_id, page_id_t parent_id = INVALID_PAGE_ID) noexcept;

    [[nodiscard]] bool is_leaf() const noexcept;
    [[nodiscard]] bool is_internal() const noexcept;
    [[nodiscard]] bool is_root() const noexcept;
    void set_is_root(bool root) noexcept;

    [[nodiscard]] uint16_t entry_count() const noexcept;
    [[nodiscard]] page_id_t parent_page_id() const noexcept;
    void set_parent_page_id(page_id_t pid) noexcept;

    // Leaf sibling links
    [[nodiscard]] page_id_t next_page_id() const noexcept;
    void set_next_page_id(page_id_t pid) noexcept;

    [[nodiscard]] page_id_t prev_page_id() const noexcept;
    void set_prev_page_id(page_id_t pid) noexcept;

    // Internal node first child
    [[nodiscard]] page_id_t first_child_id() const noexcept;
    void set_first_child_id(page_id_t pid) noexcept;

    // --- Leaf Node Entry Operations ---
    [[nodiscard]] Result<IndexEntry> get_leaf_entry(uint16_t index) const;
    [[nodiscard]] Result<IndexKey> get_leaf_key(uint16_t index) const;
    [[nodiscard]] Result<RecordId> get_leaf_rid(uint16_t index) const;

    /// Finds insertion index for (key, rid) using binary search.
    [[nodiscard]] uint16_t find_leaf_entry_index(const IndexKey& key, const RecordId& rid) const;

    /// Inserts (key, rid) into leaf node at the correct sorted position.
    Status insert_leaf_entry(const IndexKey& key, const RecordId& rid);

    /// Deletes the entry at the specified index from the leaf.
    Status remove_leaf_entry(uint16_t index);

    // --- Internal Node Entry Operations ---
    [[nodiscard]] Result<IndexKey> get_internal_key(uint16_t index) const;
    [[nodiscard]] Result<page_id_t> get_internal_child_id(uint16_t index) const;

    /// Finds the child page ID to descend into for a search key.
    [[nodiscard]] Result<page_id_t> find_child_page_id(const IndexKey& key) const;

    /// Inserts separator key and right child page ID in sorted position.
    Status insert_internal_entry(const IndexKey& key, page_id_t right_child_id);

    /// Deletes entry at index from internal node.
    Status remove_internal_entry(uint16_t index);

    // --- Capacity & Compaction ---
    [[nodiscard]] size_t free_space() const noexcept;
    [[nodiscard]] bool can_fit_leaf_entry(const IndexKey& key) const noexcept;
    [[nodiscard]] bool can_fit_internal_entry(const IndexKey& key) const noexcept;

    Status compact();

    [[nodiscard]] Page& page() noexcept { return page_; }
    [[nodiscard]] const Page& page() const noexcept { return page_; }

private:
    [[nodiscard]] uint8_t* payload() noexcept;
    [[nodiscard]] const uint8_t* payload() const noexcept;

    [[nodiscard]] uint16_t get_offset(uint16_t index) const noexcept;
    void set_offset(uint16_t index, uint16_t offset) noexcept;
    void set_entry_count(uint16_t count) noexcept;
    [[nodiscard]] uint16_t get_fsp() const noexcept;
    void set_fsp(uint16_t fsp) noexcept;

    Page& page_;
};

} // namespace klyrdb
