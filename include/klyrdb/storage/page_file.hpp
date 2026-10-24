#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/storage/page_constants.hpp"
#include "klyrdb/storage/page.hpp"
#include "klyrdb/storage/metadata_page.hpp"
#include "klyrdb/concurrency/process_file_lock.hpp"
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

namespace klyrdb {

/// Manages direct page-level persistence to a database file on disk.
///
/// Design & Constraints:
/// - Thread-safe within a process via internal I/O mutex.
/// - Cross-process safe via ProcessFileLock on companion lock file.
/// - Manages file resources using RAII.
/// - Does not overwrite existing database files upon creation.
/// - All reads and writes are page-aligned (multiples of 4096 bytes).
/// - Validates file length, header signatures, and page checksums upon access.
class PageFile {
public:
    /// Factory: creates a brand-new database file and initializes Page 0.
    /// Fails with FileAlreadyExists if the target path exists, or DatabaseBusy if locked by another process.
    [[nodiscard]] static Result<std::unique_ptr<PageFile>> create(const std::filesystem::path& path, bool lock_process = true);

    /// Factory: opens an existing database file and verifies Page 0.
    /// Fails with FileNotFound if the path does not exist, DatabaseBusy if locked, or TruncatedFile / PageCorrupted.
    [[nodiscard]] static Result<std::unique_ptr<PageFile>> open(const std::filesystem::path& path, bool lock_process = true);

    ~PageFile();

    // Move-only semantics (unique file ownership)
    PageFile(PageFile&&) noexcept;
    PageFile& operator=(PageFile&&) noexcept;
    PageFile(const PageFile&) = delete;
    PageFile& operator=(const PageFile&) = delete;

    /// Reads a page from disk by its logical page ID.
    [[nodiscard]] Status read_page(page_id_t page_id, Page& out_page);

    /// Writes an existing page to disk at its designated page offset.
    [[nodiscard]] Status write_page(const Page& page);

    /// Appends a new page to the end of the database file, updating Page 0 metadata.
    /// Sets the page's ID to the newly allocated ID and returns it.
    [[nodiscard]] Result<page_id_t> append_page(Page& page);

    /// Returns the total number of complete pages in the database file.
    [[nodiscard]] uint32_t page_count() const noexcept { return metadata_.page_count; }

    /// Returns the database metadata.
    [[nodiscard]] const DatabaseMetadata& metadata() const noexcept { return metadata_; }

    /// Updates and persists the database metadata (Page 0).
    [[nodiscard]] Status update_metadata(const DatabaseMetadata& metadata);

    /// Returns the database file path.
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    /// Flushes all pending writes to disk.
    [[nodiscard]] Status sync();

    /// Closes the database file explicitly.
    [[nodiscard]] Status close();

    [[nodiscard]] bool is_open() const noexcept { return file_stream_.is_open(); }

private:
    explicit PageFile(std::filesystem::path path, bool lock_process = true) noexcept;

    std::filesystem::path path_;
    bool lock_process_{true};
    std::unique_ptr<ProcessFileLock> proc_lock_;
    mutable std::mutex io_mutex_;
    std::fstream file_stream_;
    DatabaseMetadata metadata_;
};

} // namespace klyrdb
