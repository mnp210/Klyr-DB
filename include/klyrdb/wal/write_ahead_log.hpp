#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/wal/log_record.hpp"
#include <filesystem>
#include <memory>
#include <vector>
#include <cstdio>
#include <mutex>

namespace klyrdb {

inline constexpr uint32_t WAL_FILE_MAGIC = 0x57414C31U; // ASCII "WAL1"
inline constexpr uint16_t WAL_FILE_VERSION = 1;
inline constexpr size_t WAL_HEADER_SIZE = 32;

struct WalFileHeader {
    uint32_t magic{WAL_FILE_MAGIC};
    uint16_t version{WAL_FILE_VERSION};
    uint16_t page_size{static_cast<uint16_t>(PAGE_SIZE)};
    lsn_t checkpoint_lsn{0};
    lsn_t last_lsn{0};
    uint32_t checksum{0};

    [[nodiscard]] Status validate() const noexcept;
    void encode(std::span<uint8_t, WAL_HEADER_SIZE> dest) const noexcept;
    [[nodiscard]] static Result<WalFileHeader> decode(std::span<const uint8_t, WAL_HEADER_SIZE> src) noexcept;
};

/// Persistent Write-Ahead Log engine managing log file storage, LSN sequencing,
/// durable hardware flushes, and crash-safe record reading.
class WriteAheadLog {
public:
    explicit WriteAheadLog(std::filesystem::path path) noexcept;
    ~WriteAheadLog();

    WriteAheadLog(WriteAheadLog&&) noexcept;
    WriteAheadLog& operator=(WriteAheadLog&&) noexcept;
    WriteAheadLog(const WriteAheadLog&) = delete;
    WriteAheadLog& operator=(const WriteAheadLog&) = delete;

    /// Opens an existing WAL or creates a new one at the designated path.
    [[nodiscard]] static Result<std::unique_ptr<WriteAheadLog>> open_or_create(const std::filesystem::path& path);

    /// Appends a log record to the log buffer, assigning a monotonic LSN if not set.
    Result<lsn_t> append_record(LogRecord& record);

    /// Flushes all buffered log writes to disk with hardware durability (FlushFileBuffers / fsync).
    Status flush();

    /// Flushes the log up to at least target_lsn. Returns Ok if flushed_lsn >= target_lsn.
    Status flush_up_to(lsn_t target_lsn);

    /// Reads all valid records sequentially from the WAL file.
    /// Safely handles crashed trailing incomplete records.
    Result<std::vector<LogRecord>> read_all_records();

    /// Updates the in-place WAL header with the new checkpoint LSN and flushes to disk.
    Status update_checkpoint_boundary(lsn_t checkpoint_lsn);

    /// Truncates the WAL file, resetting to an empty log with updated checkpoint LSN.
    Status truncate(lsn_t checkpoint_lsn = 0);

    /// Atomically and safely truncates or recycles the WAL file, preserving all records
    /// with LSN >= min_retained_lsn.
    Status truncate_safe(lsn_t checkpoint_lsn, lsn_t min_retained_lsn = INVALID_LSN);

    /// Closes the WAL file.
    Status close();

    [[nodiscard]] lsn_t last_lsn() const noexcept { return last_lsn_; }
    [[nodiscard]] lsn_t flushed_lsn() const noexcept { return flushed_lsn_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }
    [[nodiscard]] size_t file_size() const;

private:
    Status write_header(lsn_t checkpoint_lsn, lsn_t last_lsn);

    mutable std::recursive_mutex wal_mutex_;
    std::filesystem::path path_;
    std::FILE* file_{nullptr};
    lsn_t last_lsn_{0};
    lsn_t flushed_lsn_{0};
    std::vector<uint8_t> write_buffer_;
};

} // namespace klyrdb
