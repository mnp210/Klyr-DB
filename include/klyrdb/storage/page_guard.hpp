#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/storage/page.hpp"

namespace klyrdb {

class BufferPoolManager;

/// RAII guard for pinned pages in the BufferPoolManager.
/// Automatically unpins the page upon destruction, preventing pin leaks.
class PageGuard {
public:
    PageGuard() noexcept = default;
    PageGuard(BufferPoolManager* bpm, page_id_t page_id, Page* page) noexcept
        : bpm_(bpm), page_id_(page_id), page_(page), is_dirty_(false) {}

    ~PageGuard();

    // Move-only semantics
    PageGuard(PageGuard&& other) noexcept;
    PageGuard& operator=(PageGuard&& other) noexcept;
    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;

    [[nodiscard]] bool is_valid() const noexcept { return bpm_ != nullptr && page_ != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return is_valid(); }

    [[nodiscard]] page_id_t page_id() const noexcept { return page_id_; }
    [[nodiscard]] Page* get() noexcept { return page_; }
    [[nodiscard]] const Page* get() const noexcept { return page_; }

    [[nodiscard]] Page& operator*() noexcept { return *page_; }
    [[nodiscard]] const Page& operator*() const noexcept { return *page_; }
    [[nodiscard]] Page* operator->() noexcept { return page_; }
    [[nodiscard]] const Page* operator->() const noexcept { return page_; }

    void mark_dirty();
    [[nodiscard]] bool is_dirty() const noexcept { return is_dirty_; }

    /// Explicitly unpins the page and disarms the guard early.
    void drop();

private:
    BufferPoolManager* bpm_{nullptr};
    page_id_t page_id_{INVALID_PAGE_ID};
    Page* page_{nullptr};
    bool is_dirty_{false};
};

} // namespace klyrdb
