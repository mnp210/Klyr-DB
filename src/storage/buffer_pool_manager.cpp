#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/recovery/crash_simulator.hpp"
#include "klyrdb/diagnostics/metrics.hpp"
#include <algorithm>

namespace klyrdb {

// --- PageGuard Implementation ---

PageGuard::~PageGuard() {
    try {
        drop();
    } catch (...) {
        // Suppress any exceptions in destructor to prevent std::terminate
    }
}

PageGuard::PageGuard(PageGuard&& other) noexcept
    : bpm_(other.bpm_), page_id_(other.page_id_), page_(other.page_), is_dirty_(other.is_dirty_) {
    other.bpm_ = nullptr;
    other.page_ = nullptr;
    other.page_id_ = INVALID_PAGE_ID;
    other.is_dirty_ = false;
}

PageGuard& PageGuard::operator=(PageGuard&& other) noexcept {
    if (this != &other) {
        drop();
        bpm_ = other.bpm_;
        page_id_ = other.page_id_;
        page_ = other.page_;
        is_dirty_ = other.is_dirty_;
        other.bpm_ = nullptr;
        other.page_ = nullptr;
        other.page_id_ = INVALID_PAGE_ID;
        other.is_dirty_ = false;
    }
    return *this;
}

void PageGuard::mark_dirty() {
    is_dirty_ = true;
    if (bpm_ != nullptr && page_id_ != INVALID_PAGE_ID) {
        bpm_->notify_page_modified(page_id_);
    }
}

void PageGuard::drop() {
    if (bpm_ != nullptr && page_id_ != INVALID_PAGE_ID) {
        try {
            (void)bpm_->unpin_page(page_id_, is_dirty_);
        } catch (...) {
        }
        bpm_ = nullptr;
        page_ = nullptr;
        page_id_ = INVALID_PAGE_ID;
        is_dirty_ = false;
    }
}

// --- BufferPoolManager Implementation ---

BufferPoolManager::BufferPoolManager(PageFile& page_file, size_t pool_size)
    : page_file_(page_file),
      pool_size_(std::max<size_t>(1, pool_size)),
      frames_(pool_size_),
      clock_hand_(0) {
    free_list_.reserve(pool_size_);
    // Initialize free frames in reverse order so 0 is popped first
    for (size_t i = pool_size_; i > 0; --i) {
        free_list_.push_back(i - 1);
    }
}

BufferPoolManager::~BufferPoolManager() {
    try {
        (void)flush_all_pages();
    } catch (...) {
        // Suppress any crash simulation or I/O exceptions in destructor
    }
}

BufferPoolManager::BufferPoolManager(BufferPoolManager&& other) noexcept
    : page_file_(other.page_file_),
      wal_(other.wal_),
      txn_mgr_(other.txn_mgr_),
      pool_size_(other.pool_size_),
      frames_(std::move(other.frames_)),
      free_list_(std::move(other.free_list_)),
      page_table_(std::move(other.page_table_)),
      clock_hand_(other.clock_hand_) {
    other.wal_ = nullptr;
    other.txn_mgr_ = nullptr;
}

BufferPoolManager& BufferPoolManager::operator=(BufferPoolManager&& other) noexcept {
    if (this != &other) {
        try {
            (void)flush_all_pages();
        } catch (...) {
        }
        wal_ = other.wal_;
        txn_mgr_ = other.txn_mgr_;
        pool_size_ = other.pool_size_;
        frames_ = std::move(other.frames_);
        free_list_ = std::move(other.free_list_);
        page_table_ = std::move(other.page_table_);
        clock_hand_ = other.clock_hand_;
        other.wal_ = nullptr;
        other.txn_mgr_ = nullptr;
    }
    return *this;
}

bool BufferPoolManager::find_victim_frame(frame_id_t& out_frame_id) {
    if (pool_size_ == 0) {
        return false;
    }

    // At most 2 sweeps through all frames in the buffer pool
    const size_t max_scans = 2 * pool_size_;
    for (size_t i = 0; i < max_scans; ++i) {
        frame_id_t fid = clock_hand_;
        clock_hand_ = (clock_hand_ + 1) % pool_size_;

        if (!frames_[fid].is_valid) {
            continue;
        }

        if (frames_[fid].pin_count == 0) {
            if (frames_[fid].reference_bit) {
                // Second chance: clear reference bit
                frames_[fid].reference_bit = false;
            } else {
                // Victim chosen!
                out_frame_id = fid;
                return true;
            }
        }
    }

    return false;
}

Result<Page*> BufferPoolManager::fetch_page(page_id_t page_id) {
    std::lock_guard<std::recursive_mutex> lock(bpm_mutex_);
    if (metrics_ != nullptr) {
        metrics_->record_page_request();
    }
    if (page_id >= page_file_.page_count()) {
        return Status::invalid_page_id("Page ID " + std::to_string(page_id) +
                                       " is out of range [0, " +
                                       std::to_string(page_file_.page_count()) + ")");
    }

    // 1. Check if page is already cached in memory
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        const frame_id_t fid = it->second;
        frames_[fid].pin_count++;
        frames_[fid].reference_bit = true;
        if (metrics_ != nullptr) {
            metrics_->record_cache_hit();
            metrics_->update_pinned_frames(pinned_frame_count());
        }
        return &frames_[fid].page;
    }

    if (metrics_ != nullptr) {
        metrics_->record_cache_miss();
    }

    // 2. Select a frame for loading
    frame_id_t fid = 0;
    if (!free_list_.empty()) {
        fid = free_list_.back();
        free_list_.pop_back();
    } else {
        if (!find_victim_frame(fid)) {
            if (metrics_ != nullptr) {
                metrics_->record_allocation_failure();
            }
            return Status::buffer_pool_full("All " + std::to_string(pool_size_) +
                                            " buffer pool frames are currently pinned");
        }

        // Evict chosen victim frame
        if (frames_[fid].is_dirty) {
            uint32_t p_lsn = frames_[fid].page.page_lsn();
            if (wal_ != nullptr && p_lsn > wal_->flushed_lsn()) {
                Status ws = wal_->flush_up_to(p_lsn);
                if (!ws.is_ok()) {
                    return ws;
                }
            }
            Status flush_s = page_file_.write_page(frames_[fid].page);
            if (!flush_s.is_ok()) {
                return flush_s; // Never overwrite dirty data if flush fails
            }
            if (metrics_ != nullptr) {
                metrics_->record_physical_write();
                metrics_->record_page_flush();
            }
            frames_[fid].is_dirty = false;
        }
        if (metrics_ != nullptr) {
            metrics_->record_eviction();
        }
        frames_[fid].has_before_image = false;
        page_table_.erase(frames_[fid].page_id);
    }

    // 3. Read page from disk into frame
    Status read_s = page_file_.read_page(page_id, frames_[fid].page);
    if (!read_s.is_ok()) {
        // Return frame to free list if load failed
        frames_[fid].is_valid = false;
        frames_[fid].has_before_image = false;
        free_list_.push_back(fid);
        return read_s;
    }
    if (metrics_ != nullptr) {
        metrics_->record_physical_read();
    }

    // 4. Update frame metadata
    frames_[fid].page_id = page_id;
    frames_[fid].pin_count = 1;
    frames_[fid].is_dirty = false;
    frames_[fid].is_valid = true;
    frames_[fid].reference_bit = true;
    frames_[fid].before_image = frames_[fid].page;
    frames_[fid].has_before_image = true;
    page_table_[page_id] = fid;

    if (metrics_ != nullptr) {
        metrics_->update_pinned_frames(pinned_frame_count());
    }

    return &frames_[fid].page;
}

Result<PageGuard> BufferPoolManager::fetch_page_guard(page_id_t page_id) {
    auto res = fetch_page(page_id);
    if (res.is_err()) {
        return res.status();
    }
    return PageGuard(this, page_id, res.value());
}

Result<Page*> BufferPoolManager::new_page(page_id_t& out_page_id) {
    std::lock_guard<std::recursive_mutex> lock(bpm_mutex_);
    if (metrics_ != nullptr) {
        metrics_->record_page_request();
    }
    // 1. Pre-check frame availability before appending to disk
    frame_id_t fid = 0;
    if (!free_list_.empty()) {
        fid = free_list_.back();
        free_list_.pop_back();
    } else {
        if (!find_victim_frame(fid)) {
            if (metrics_ != nullptr) {
                metrics_->record_allocation_failure();
            }
            return Status::buffer_pool_full("Cannot allocate new page: all buffer pool frames pinned");
        }

        // Evict chosen victim frame
        if (frames_[fid].is_dirty) {
            Status flush_s = page_file_.write_page(frames_[fid].page);
            if (!flush_s.is_ok()) {
                return flush_s;
            }
            if (metrics_ != nullptr) {
                metrics_->record_physical_write();
                metrics_->record_page_flush();
            }
            frames_[fid].is_dirty = false;
        }
        if (metrics_ != nullptr) {
            metrics_->record_eviction();
        }
        page_table_.erase(frames_[fid].page_id);
    }

    // 2. Allocate new page on disk
    Page blank_page;
    blank_page.set_page_type(PageType::Record);
    auto append_res = page_file_.append_page(blank_page);
    if (append_res.is_err()) {
        frames_[fid].is_valid = false;
        free_list_.push_back(fid);
        return append_res.status();
    }
    if (metrics_ != nullptr) {
        metrics_->record_physical_write();
    }

    const page_id_t new_pid = append_res.value();
    out_page_id = new_pid;

    // 3. Initialize frame
    frames_[fid].page = blank_page;
    frames_[fid].page.set_page_id(new_pid);
    frames_[fid].before_image = blank_page;
    frames_[fid].has_before_image = true;
    frames_[fid].page_id = new_pid;
    frames_[fid].pin_count = 1;
    frames_[fid].is_dirty = true;
    frames_[fid].is_valid = true;
    frames_[fid].reference_bit = true;
    page_table_[new_pid] = fid;

    if (metrics_ != nullptr) {
        metrics_->update_pinned_frames(pinned_frame_count());
    }

    return &frames_[fid].page;
}

Result<PageGuard> BufferPoolManager::new_page_guard(page_id_t& out_page_id) {
    auto res = new_page(out_page_id);
    if (res.is_err()) {
        return res.status();
    }
    return PageGuard(this, out_page_id, res.value());
}

Status BufferPoolManager::unpin_page(page_id_t page_id, bool is_dirty) {
    std::lock_guard<std::recursive_mutex> lock(bpm_mutex_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return Status::invalid_page_id("Page " + std::to_string(page_id) + " is not in buffer pool");
    }

    const frame_id_t fid = it->second;
    if (frames_[fid].pin_count == 0) {
        return Status::invalid_argument("Cannot unpin page " + std::to_string(page_id) +
                                       " with pin_count == 0");
    }

    if (is_dirty) {
        frames_[fid].is_dirty = true;
    }

    frames_[fid].pin_count--;
    if (metrics_ != nullptr) {
        metrics_->update_pinned_frames(pinned_frame_count());
    }
    return Status::ok();
}

void BufferPoolManager::notify_page_modified(page_id_t page_id) {
    std::lock_guard<std::recursive_mutex> lock(bpm_mutex_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return;
    }

    const frame_id_t fid = it->second;
    frames_[fid].is_dirty = true;
    if (txn_mgr_ != nullptr) {
        auto txn = txn_mgr_->current_transaction();
        if (txn != nullptr && txn->is_active()) {
            if (txn->is_rolling_back()) {
                frames_[fid].before_image = frames_[fid].page;
            } else if (frames_[fid].has_before_image) {
                (void)txn_mgr_->log_page_update(txn, page_id, frames_[fid].before_image, frames_[fid].page);
                frames_[fid].before_image = frames_[fid].page;
            }
        }
    }
}

Status BufferPoolManager::flush_page(page_id_t page_id) {
    std::lock_guard<std::recursive_mutex> lock(bpm_mutex_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return Status::ok(); // Page not in cache; already persisted
    }

    const frame_id_t fid = it->second;
    if (frames_[fid].is_dirty) {
        uint32_t p_lsn = frames_[fid].page.page_lsn();
        if (wal_ != nullptr && p_lsn > wal_->flushed_lsn()) {
            Status ws = wal_->flush_up_to(p_lsn);
            if (!ws.is_ok()) {
                return ws;
            }
        }
        CrashSimulator::trigger(CrashPoint::DuringPageFlush);
        Status s = page_file_.write_page(frames_[fid].page);
        if (!s.is_ok()) {
            return s;
        }
        if (metrics_ != nullptr) {
            metrics_->record_physical_write();
            metrics_->record_page_flush();
        }
        frames_[fid].is_dirty = false;
    }
    return Status::ok();
}

Status BufferPoolManager::flush_all_pages() {
    std::lock_guard<std::recursive_mutex> lock(bpm_mutex_);
    if (wal_ != nullptr) {
        Status ws = wal_->flush();
        if (!ws.is_ok()) {
            return ws;
        }
    }
    size_t flushed_count = 0;
    for (size_t fid = 0; fid < pool_size_; ++fid) {
        if (frames_[fid].is_valid && frames_[fid].is_dirty) {
            CrashSimulator::trigger(CrashPoint::DuringPageFlush);
            Status s = page_file_.write_page(frames_[fid].page);
            if (!s.is_ok()) {
                return s;
            }
            if (metrics_ != nullptr) {
                metrics_->record_physical_write();
                metrics_->record_page_flush();
            }
            frames_[fid].is_dirty = false;
            flushed_count++;
            if (flushed_count == 1) {
                CrashSimulator::trigger(CrashPoint::AfterOnePageFlushed);
            }
        }
    }
    return page_file_.sync();
}

Status BufferPoolManager::delete_page(page_id_t page_id) {
    std::lock_guard<std::recursive_mutex> lock(bpm_mutex_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return Status::ok();
    }

    const frame_id_t fid = it->second;
    if (frames_[fid].pin_count > 0) {
        return Status::invalid_argument("Cannot delete pinned page " + std::to_string(page_id));
    }

    page_table_.erase(it);
    frames_[fid].page_id = INVALID_PAGE_ID;
    frames_[fid].pin_count = 0;
    frames_[fid].is_dirty = false;
    frames_[fid].is_valid = false;
    frames_[fid].has_before_image = false;
    frames_[fid].reference_bit = false;
    free_list_.push_back(fid);

    return Status::ok();
}

void BufferPoolManager::drop_frames_without_flush() noexcept {
    std::lock_guard<std::recursive_mutex> lock(bpm_mutex_);
    for (auto& frame : frames_) {
        frame.is_dirty = false;
        frame.is_valid = false;
        frame.has_before_image = false;
        frame.pin_count = 0;
        frame.page_id = INVALID_PAGE_ID;
    }
    page_table_.clear();
    free_list_.clear();
    for (size_t i = pool_size_; i > 0; --i) {
        free_list_.push_back(i - 1);
    }
    clock_hand_ = 0;
}

size_t BufferPoolManager::pinned_frame_count() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(bpm_mutex_);
    size_t count = 0;
    for (const auto& frame : frames_) {
        if (frame.is_valid && frame.pin_count > 0) {
            count++;
        }
    }
    return count;
}

bool BufferPoolManager::is_page_in_memory(page_id_t page_id) const noexcept {
    std::lock_guard<std::recursive_mutex> lock(bpm_mutex_);
    return page_table_.find(page_id) != page_table_.end();
}

uint32_t BufferPoolManager::pin_count(page_id_t page_id) const noexcept {
    std::lock_guard<std::recursive_mutex> lock(bpm_mutex_);
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        return frames_[it->second].pin_count;
    }
    return 0;
}

bool BufferPoolManager::is_dirty(page_id_t page_id) const noexcept {
    std::lock_guard<std::recursive_mutex> lock(bpm_mutex_);
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        return frames_[it->second].is_dirty;
    }
    return false;
}

} // namespace klyrdb
