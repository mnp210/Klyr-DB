#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include <string>
#include <vector>
#include <filesystem>
#include <cstdint>
#include <string_view>
#include <optional>

namespace klyrdb {

/// Represents an individual physical file component stored in a backup.
struct BackupComponent {
    std::string name;       // Logical identifier ("database", "wal")
    std::string filename;   // Base filename ("mydb.kdb", "mydb.kdb.wal")
    uint64_t size_bytes{0}; // Exact file size in bytes
    uint32_t crc32{0};      // CRC-32 checksum (IEEE 802.3 standard)
};

/// Versioned metadata manifest describing a physical backup.
struct BackupManifest {
    static constexpr uint32_t CURRENT_BACKUP_FORMAT_VERSION = 1;
    static constexpr uint16_t CURRENT_STORAGE_FORMAT_VERSION = 1;

    uint32_t backup_format_version{CURRENT_BACKUP_FORMAT_VERSION};
    uint16_t storage_format_version{CURRENT_STORAGE_FORMAT_VERSION};
    std::string backup_type{"PhysicalFull"};
    std::string source_db_name;
    std::string created_at_utc;
    uint64_t created_at_epoch{0};
    uint32_t page_size{4096};
    uint32_t page_count{0};
    lsn_t checkpoint_lsn{0};
    lsn_t min_required_lsn{0};
    std::string status{"Complete"}; // "Complete", "Incomplete"
    std::string checksum_algo{"CRC32"};
    std::vector<BackupComponent> components;

    [[nodiscard]] const BackupComponent* find_component(std::string_view name) const;
    [[nodiscard]] std::string to_json() const;
    static Result<BackupManifest> from_json(std::string_view json_str);

    [[nodiscard]] Status write_to_file(const std::filesystem::path& path) const;
    static Result<BackupManifest> read_from_file(const std::filesystem::path& path);
};

/// Computes the IEEE 802.3 CRC-32 checksum of an entire file on disk.
[[nodiscard]] Result<uint32_t> calculate_file_crc32(const std::filesystem::path& path);

/// Generates an ISO 8601 UTC timestamp string (e.g. "2026-10-23T13:20:00Z").
[[nodiscard]] std::string current_utc_timestamp_iso8601();

/// Returns the current seconds since Unix epoch.
[[nodiscard]] uint64_t current_epoch_seconds();

} // namespace klyrdb
