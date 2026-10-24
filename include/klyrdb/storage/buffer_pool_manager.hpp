#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/storage/page.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/page_guard.hpp"
#include <vector>
#include <unordered_map>
#include <cstddef>
#include <mutex>

namespace klyrdb {

using frame_id_t = size_t;

inline constexpr size_t DEFAULT_BUFFER_POOL_SIZE = 64;

class WriteAheadLog;
class TransactionManager;
class DatabaseMetrics;

/// In-memory frame holding a page and its buffer pool metadata.
struct PageFrame {
    Page page;
    Page before_image;
    bool has_before_image{false};
    page_id_t page_id{INVALID_PAGE_ID};
    uint32_t pin_count{0};
    bool is_dirty{false};
    bool is_valid{false};
    bool reference_bit{false}; // Used by Clock replacement policy
};

/// Coordinates in-memory caching of disk pages using a Clock replacement policy.
///
/// Single-process and single-writer in Phase 2.
class BufferPoolManager {
public:
    explicit BufferPoolManager(PageFile& page_file, size_t pool_size = DEFAULT_BUFFER_POOL_SIZE);
    ~BufferPoolManager();

    // Move-only semantics (tied to specific PageFile)
    BufferPoolManager(BufferPoolManager&&) noexcept;
    BufferPoolManager& operator=(BufferPoolManager&&) noexcept;
    BufferPoolManager(const BufferPoolManager&) = delete;
    BufferPoolManager& operator=(const BufferPoolManager&) = delete;

    /// Fetches a page into memory and increments its pin count.
    Result<Page*> fetch_page(page_id_t page_id);

    /// Fetches a page returning an RAII PageGuard.
    Result<PageGuard> fetch_page_guard(page_id_t page_id);

    /// Allocates a new page on disk and brings it pinned into the buffer pool.
    Result<Page*> new_page(page_id_t& out_page_id);

    /// Allocates a new page returning an RAII PageGuard.
    Result<PageGuard> new_page_guard(page_id_t& out_page_id);

    /// Decrements the pin count of a cached page.
    Status unpin_page(page_id_t page_id, bool is_dirty);

    /// Writes a specific page to disk if dirty and clears its dirty flag.
    Status flush_page(page_id_t page_id);

    /// Writes all dirty cached pages to disk and syncs the underlying file.
    Status flush_all_pages();

    /// Deletes a page from the buffer pool if its pin count is 0.
    Status delete_page(page_id_t page_id);

    /// Discards all in-memory frames without flushing them to disk.
    /// Used by crash simulation to mimic process kill / power failure.
    void drop_frames_without_flush() noexcept;

    // --- State and Diagnostics ---
    [[nodiscard]] size_t pool_size() const noexcept { return pool_size_; }
    [[nodiscard]] size_t free_frame_count() const noexcept { return free_list_.size(); }
    [[nodiscard]] size_t pinned_frame_count() const noexcept;
    [[nodiscard]] bool is_page_in_memory(page_id_t page_id) const noexcept;
    [[nodiscard]] uint32_t pin_count(page_id_t page_id) const noexcept;
    [[nodiscard]] bool is_dirty(page_id_t page_id) const noexcept;

    [[nodiscard]] PageFile& page_file() noexcept { return page_file_; }
    [[nodiscard]] const PageFile& page_file() const noexcept { return page_file_; }

    void set_wal(WriteAheadLog* wal) noexcept { wal_ = wal; }
    [[nodiscard]] WriteAheadLog* wal() const noexcept { return wal_; }

    void set_transaction_manager(TransactionManager* tm) noexcept { txn_mgr_ = tm; }
    [[nodiscard]] TransactionManager* transaction_manager() const noexcept { return txn_mgr_; }

    void set_metrics(DatabaseMetrics* metrics) noexcept { metrics_ = metrics; }
    [[nodiscard]] DatabaseMetrics* metrics() const noexcept { return metrics_; }

    /// Notifies buffer pool that a page has been modified under active transaction.
    void notify_page_modified(page_id_t page_id);

private:
    [[nodiscard]] bool find_victim_frame(frame_id_t& out_frame_id);

    mutable std::recursive_mutex bpm_mutex_;
    PageFile& page_file_;
    WriteAheadLog* wal_{nullptr};
    TransactionManager* txn_mgr_{nullptr};
    DatabaseMetrics* metrics_{nullptr};
    size_t pool_size_;
    std::vector<PageFrame> frames_;
    std::vector<frame_id_t> free_list_;
    std::unordered_map<page_id_t, frame_id_t> page_table_;
    frame_id_t clock_hand_{0};
};

} // namespace klyrdb
