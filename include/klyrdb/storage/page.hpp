#pragma once

#include "klyrdb/common/types.hpp"
#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/storage/page_constants.hpp"
#include "klyrdb/storage/page_header.hpp"
#include <array>
#include <span>

namespace klyrdb {

/// Represents an in-memory 4096-byte database page.
/// Encapsulates page metadata and ensures bounds-checked payload operations.
class Page {
public:
    /// Default constructor creates a valid empty Data page.
    Page() noexcept;

    /// Constructs a page with a specific ID and type.
    explicit Page(page_id_t page_id, PageType type = PageType::Data) noexcept;

    // --- Header Accessors & Mutators ---
    [[nodiscard]] page_id_t page_id() const noexcept { return header_.page_id; }
    void set_page_id(page_id_t id) noexcept { header_.page_id = id; sync_header_to_raw(); }

    [[nodiscard]] PageType page_type() const noexcept { return header_.page_type; }
    void set_page_type(PageType type) noexcept { header_.page_type = type; sync_header_to_raw(); }

    [[nodiscard]] uint16_t payload_size() const noexcept { return header_.payload_size; }
    void set_payload_size(uint16_t size) noexcept { header_.payload_size = size; sync_header_to_raw(); }
    [[nodiscard]] uint16_t version() const noexcept { return header_.version; }
    [[nodiscard]] uint32_t checksum() const noexcept { return header_.checksum; }
    [[nodiscard]] uint32_t page_lsn() const noexcept { return header_.page_lsn; }
    void set_page_lsn(uint32_t lsn) noexcept { header_.page_lsn = lsn; sync_header_to_raw(); }
    [[nodiscard]] const PageHeader& header() const noexcept { return header_; }

    // --- Payload Accessors & Mutators ---
    /// Returns a read-only view of the active payload area.
    [[nodiscard]] std::span<const uint8_t> payload() const noexcept;

    /// Sets the entire payload, replacing any existing content.
    /// Fails with PayloadOverflow if data.size() > MAX_PAYLOAD_SIZE (4072 bytes).
    Status set_payload(std::span<const uint8_t> data) noexcept;

    /// Writes data at a specific offset within the payload area.
    /// Fails with PayloadOverflow if offset + data.size() > MAX_PAYLOAD_SIZE.
    Status write_payload(size_t offset, std::span<const uint8_t> data) noexcept;

    /// Clears the payload to zero and resets payload_size to 0.
    void clear_payload() noexcept;

    // --- Serialization & Deserialization ---
    /// Serializes the page into exactly 4096 bytes, updating the CRC-32 checksum.
    Status serialize(std::span<uint8_t, PAGE_SIZE> dest) noexcept;

    /// Deserializes a page from a 4096-byte buffer, verifying header and CRC-32 checksum.
    Status deserialize(std::span<const uint8_t, PAGE_SIZE> src) noexcept;

    /// Loads the page from a 4096-byte raw image (copying data and decoding header).
    void load_from_raw(std::span<const uint8_t, PAGE_SIZE> src) noexcept;

    /// Synchronizes the in-memory PageHeader into the first 24 bytes of data_.
    void sync_header_to_raw() noexcept {
        header_.encode(std::span<uint8_t, PAGE_HEADER_SIZE>(data_.data(), PAGE_HEADER_SIZE));
    }

    /// Access the underlying 4096-byte raw memory buffer.
    [[nodiscard]] std::span<const uint8_t, PAGE_SIZE> raw_data() const noexcept {
        const_cast<Page*>(this)->sync_header_to_raw();
        return data_;
    }

    /// Access the underlying 4096-byte raw memory buffer for direct in-place mutation.
    [[nodiscard]] std::span<uint8_t, PAGE_SIZE> raw_data_mut() noexcept {
        sync_header_to_raw();
        return data_;
    }

    /// Access the mutable payload area (4072 bytes).
    [[nodiscard]] std::span<uint8_t> mutable_payload() noexcept {
        return std::span<uint8_t>(data_.data() + OFFSET_PAGE_PAYLOAD, MAX_PAYLOAD_SIZE);
    }

private:
    PageHeader header_;
    std::array<uint8_t, PAGE_SIZE> data_{};
};

} // namespace klyrdb
