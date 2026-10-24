#include "klyrdb/common/checksum.hpp"
#include <array>

namespace klyrdb {

namespace {

/// Generates the standard CRC-32 (IEEE 802.3 reversed polynomial 0xEDB88320) lookup table.
consteval auto generate_crc32_table() noexcept {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320U;
            } else {
                crc >>= 1;
            }
        }
        table[i] = crc;
    }
    return table;
}

// Compile-time evaluated lookup table
constexpr auto CRC32_TABLE = generate_crc32_table();

} // namespace

uint32_t calculate_crc32(std::span<const uint8_t> data, uint32_t seed) noexcept {
    uint32_t crc = ~seed;
    for (const uint8_t byte : data) {
        const uint8_t index = static_cast<uint8_t>((crc ^ byte) & 0xFF);
        crc = (crc >> 8) ^ CRC32_TABLE[index];
    }
    return ~crc;
}

} // namespace klyrdb
