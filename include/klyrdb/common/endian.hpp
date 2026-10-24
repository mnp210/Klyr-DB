#pragma once

#include <cstdint>
#include <cstring>
#include <bit>

namespace klyrdb {

/// Explicit Big-Endian (Network Byte Order) Encoding & Decoding
///
/// We deliberately use bit-shifting routines rather than host-endian reinterpret_cast
/// or raw struct serialization. This ensures:
/// 1. Complete independence from the host architecture's native byte order (x86_64, ARM, etc.).
/// 2. Elimination of compiler structure padding and alignment variance.
/// 3. Cross-platform file compatibility between Windows, Linux, macOS, and any future platform.

// --- 16-bit unsigned integers ---

inline void encode_u16_be(uint16_t value, uint8_t* out) noexcept {
    out[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[1] = static_cast<uint8_t>(value & 0xFF);
}

[[nodiscard]] inline uint16_t decode_u16_be(const uint8_t* in) noexcept {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(in[0]) << 8) |
        static_cast<uint16_t>(in[1])
    );
}

// --- 32-bit unsigned integers ---

inline void encode_u32_be(uint32_t value, uint8_t* out) noexcept {
    out[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(value & 0xFF);
}

[[nodiscard]] inline uint32_t decode_u32_be(const uint8_t* in) noexcept {
    return (static_cast<uint32_t>(in[0]) << 24) |
           (static_cast<uint32_t>(in[1]) << 16) |
           (static_cast<uint32_t>(in[2]) << 8)  |
           static_cast<uint32_t>(in[3]);
}

// --- 64-bit unsigned integers ---

inline void encode_u64_be(uint64_t value, uint8_t* out) noexcept {
    out[0] = static_cast<uint8_t>((value >> 56) & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 48) & 0xFF);
    out[2] = static_cast<uint8_t>((value >> 40) & 0xFF);
    out[3] = static_cast<uint8_t>((value >> 32) & 0xFF);
    out[4] = static_cast<uint8_t>((value >> 24) & 0xFF);
    out[5] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[6] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[7] = static_cast<uint8_t>(value & 0xFF);
}

[[nodiscard]] inline uint64_t decode_u64_be(const uint8_t* in) noexcept {
    return (static_cast<uint64_t>(in[0]) << 56) |
           (static_cast<uint64_t>(in[1]) << 48) |
           (static_cast<uint64_t>(in[2]) << 40) |
           (static_cast<uint64_t>(in[3]) << 32) |
           (static_cast<uint64_t>(in[4]) << 24) |
           (static_cast<uint64_t>(in[5]) << 16) |
           (static_cast<uint64_t>(in[6]) << 8)  |
           static_cast<uint64_t>(in[7]);
}

// --- Signed integer counterparts ---

inline void encode_i16_be(int16_t value, uint8_t* out) noexcept {
    encode_u16_be(std::bit_cast<uint16_t>(value), out);
}

[[nodiscard]] inline int16_t decode_i16_be(const uint8_t* in) noexcept {
    return std::bit_cast<int16_t>(decode_u16_be(in));
}

inline void encode_i32_be(int32_t value, uint8_t* out) noexcept {
    encode_u32_be(std::bit_cast<uint32_t>(value), out);
}

[[nodiscard]] inline int32_t decode_i32_be(const uint8_t* in) noexcept {
    return std::bit_cast<int32_t>(decode_u32_be(in));
}

inline void encode_i64_be(int64_t value, uint8_t* out) noexcept {
    encode_u64_be(std::bit_cast<uint64_t>(value), out);
}

[[nodiscard]] inline int64_t decode_i64_be(const uint8_t* in) noexcept {
    return std::bit_cast<int64_t>(decode_u64_be(in));
}

// --- Floating point numbers ---

inline void encode_f64_be(double value, uint8_t* out) noexcept {
    encode_u64_be(std::bit_cast<uint64_t>(value), out);
}

[[nodiscard]] inline double decode_f64_be(const uint8_t* in) noexcept {
    return std::bit_cast<double>(decode_u64_be(in));
}

} // namespace klyrdb
