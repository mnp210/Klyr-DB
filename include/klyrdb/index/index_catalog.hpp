#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/index/key.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include <string>
#include <vector>
#include <memory>
#include <string_view>

namespace klyrdb {

/// Logical descriptor for a registered B+ Tree index.
struct IndexMetadata {
    std::string name;
    KeyType key_type{KeyType::Int64};
    page_id_t root_page_id{INVALID_PAGE_ID};
    bool is_unique{false};
};

/// Persistent catalog tracking B+ tree indexes and their root page IDs.
class IndexCatalog {
public:
    explicit IndexCatalog(BufferPoolManager& bpm, page_id_t catalog_page_id) noexcept
        : bpm_(bpm), catalog_page_id_(catalog_page_id) {}

    /// Opens an existing IndexCatalog or creates a new one, updating Page 0 metadata if needed.
    static Result<std::unique_ptr<IndexCatalog>> open_or_create(BufferPoolManager& bpm);

    [[nodiscard]] page_id_t catalog_page_id() const noexcept { return catalog_page_id_; }

    /// Retrieves index metadata by name. Returns IndexNotFound if not found.
    [[nodiscard]] Result<IndexMetadata> get_index(std::string_view name);

    /// Registers a new index. Returns DuplicateKey if index name already exists.
    Status create_index(std::string_view name, KeyType key_type, page_id_t root_page_id, bool is_unique = false);

    /// Updates the root page pointer for an existing index.
    Status update_root_page(std::string_view name, page_id_t new_root_id);

    /// Removes an index from the catalog.
    Status drop_index(std::string_view name);

    /// Returns a list of all active indexes.
    [[nodiscard]] Result<std::vector<IndexMetadata>> list_indexes();

private:
    BufferPoolManager& bpm_;
    page_id_t catalog_page_id_{INVALID_PAGE_ID};
};

} // namespace klyrdb
