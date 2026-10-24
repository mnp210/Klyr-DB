#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include <memory>

namespace klyrdb {

/// Persistent Free-Space Map (FSM) recording per-page available space
/// to prevent linear file scans during record insertion.
class FreeSpaceMap {
public:
    explicit FreeSpaceMap(BufferPoolManager& bpm, page_id_t fsm_root_page_id) noexcept
        : bpm_(bpm), root_page_id_(fsm_root_page_id) {}

    /// Opens an existing FreeSpaceMap or creates a new one, persisting fsm_page_id in DatabaseMetadata.
    static Result<std::unique_ptr<FreeSpaceMap>> open_or_create(BufferPoolManager& bpm);

    [[nodiscard]] page_id_t root_page_id() const noexcept { return root_page_id_; }

    /// Finds a data page with at least required_bytes of free space.
    /// Returns NoFreeSpace status if no existing candidate page qualifies.
    [[nodiscard]] Result<page_id_t> find_page_with_space(size_t required_bytes);

    /// Updates the recorded free space for a specific page.
    Status update_free_space(page_id_t pid, size_t free_bytes);

    /// Retrieves the recorded free space for a specific page.
    [[nodiscard]] Result<size_t> get_free_space(page_id_t pid);

    /// Scans the database file to rebuild the FSM from actual page contents.
    Status rebuild();

private:
    BufferPoolManager& bpm_;
    page_id_t root_page_id_{INVALID_PAGE_ID};
};

} // namespace klyrdb
