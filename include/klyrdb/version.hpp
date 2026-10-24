#pragma once

#include <string>
#include <cstdint>

namespace klyrdb {

/// Semantic version of the KlyrDB engine.
struct Version {
    int major{0};
    int minor{12};
    int patch{0};

    [[nodiscard]] std::string to_string() const {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }
};

/// Returns the current software release version of the KlyrDB engine.
[[nodiscard]] inline Version version() noexcept {
    return Version{0, 12, 0};
}

/// Returns the current on-disk page format version.
[[nodiscard]] inline constexpr uint32_t storage_format_version() noexcept {
    return 1;
}

/// Returns the current physical backup format version.
[[nodiscard]] inline constexpr uint32_t backup_format_version() noexcept {
    return 1;
}

} // namespace klyrdb
