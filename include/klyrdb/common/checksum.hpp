#pragma once

#include <cstdint>
#include <span>

namespace klyrdb {

/// Computes a standard CRC-32 (IEEE 802.3, polynomial 0xEDB88320) checksum.
///
/// Characteristics & Limitations:
/// - CRC-32 is an error-detecting cyclic redundancy code designed to catch accidental data corruption
///   caused by storage medium degradation, network noise, or partial sector write failures.
/// - It can detect all single-bit and double-bit errors, and any burst error of length <= 32 bits.
/// - Limitation: CRC-32 is NOT a cryptographic hash (unlike SHA-256). An adversary can intentionally
///   alter database contents and craft matching CRC-32 checksums without difficulty.
///   It provides physical and logical corruption protection, not authentication or anti-tamper security.
[[nodiscard]] uint32_t calculate_crc32(std::span<const uint8_t> data, uint32_t seed = 0) noexcept;

} // namespace klyrdb
