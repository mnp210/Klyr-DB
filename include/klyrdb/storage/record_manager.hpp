#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/storage/record_id.hpp"
#include "klyrdb/storage/record.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/slotted_page.hpp"
#include <span>
#include <vector>

namespace klyrdb {

class FreeSpaceMap;

/// High-level record management engine coordinating multi-page record allocation,
/// retrieval, updates, and deletions via the buffer pool and Free-Space Map.
class RecordManager {
public:
    explicit RecordManager(BufferPoolManager& bpm, FreeSpaceMap* fsm = nullptr) noexcept
        : bpm_(bpm), fsm_(fsm) {}

    void set_fsm(FreeSpaceMap* fsm) noexcept { fsm_ = fsm; }
    [[nodiscard]] FreeSpaceMap* fsm() const noexcept { return fsm_; }

    /// Inserts an opaque record byte payload, using FSM if available to locate
    /// a candidate page without linear scans, allocating a new page if necessary.
    /// Returns the persistent RecordId.
    Result<RecordId> insert_record(std::span<const uint8_t> data);

    /// Convenience overload to insert a typed Phase 1 Record tuple.
    Result<RecordId> insert_record(const Record& record);

    /// Retrieves an opaque record byte payload by its RecordId.
    Result<std::vector<uint8_t>> get_record(RecordId rid);

    /// Convenience overload to retrieve and deserialize a typed Phase 1 Record tuple.
    Result<Record> get_typed_record(RecordId rid);

    /// Updates an existing record's contents, updating the FSM summary.
    Status update_record(RecordId rid, std::span<const uint8_t> new_data);

    /// Deletes a record, tombstoning its slot and refreshing FSM space.
    Status delete_record(RecordId rid);

    /// Returns a reference to the underlying BufferPoolManager.
    [[nodiscard]] BufferPoolManager& buffer_pool_manager() noexcept { return bpm_; }
    [[nodiscard]] const BufferPoolManager& buffer_pool_manager() const noexcept { return bpm_; }

private:
    BufferPoolManager& bpm_;
    FreeSpaceMap* fsm_{nullptr};
};

} // namespace klyrdb
