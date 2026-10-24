#define _CRT_SECURE_NO_WARNINGS
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/recovery/crash_simulator.hpp"
#include "klyrdb/common/endian.hpp"
#include "klyrdb/common/checksum.hpp"
#include <array>
#include <cstring>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace klyrdb {

namespace {

constexpr size_t OFFSET_WH_MAGIC          = 0;  // 4B: uint32_t
constexpr size_t OFFSET_WH_VERSION        = 4;  // 2B: uint16_t
constexpr size_t OFFSET_WH_PAGE_SIZE      = 6;  // 2B: uint16_t
constexpr size_t OFFSET_WH_CHECKPOINT_LSN = 8;  // 8B: uint64_t
constexpr size_t OFFSET_WH_LAST_LSN       = 16; // 8B: uint64_t
constexpr size_t OFFSET_WH_CHECKSUM       = 24; // 4B: uint32_t
constexpr size_t OFFSET_WH_RESERVED       = 28; // 4B: uint32_t

bool platform_durable_sync(std::FILE* file) {
    if (file == nullptr) return false;
    std::fflush(file);
#ifdef _WIN32
    int fd = _fileno(file);
    if (fd < 0) return false;
    HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (handle == INVALID_HANDLE_VALUE) return false;
    return FlushFileBuffers(handle) != 0;
#else
    int fd = fileno(file);
    if (fd < 0) return false;
    return fsync(fd) == 0;
#endif
}

} // namespace

Status WalFileHeader::validate() const noexcept {
    if (magic != WAL_FILE_MAGIC) {
        return Status::invalid_magic("Invalid WAL file magic signature: expected 0x57414C31 ('WAL1')");
    }
    if (version != WAL_FILE_VERSION) {
        return Status::unsupported_version("Unsupported WAL file version: " + std::to_string(version));
    }
    if (page_size != PAGE_SIZE) {
        return Status::page_corrupted("WAL page size " + std::to_string(page_size) +
                                      " does not match database page size " + std::to_string(PAGE_SIZE));
    }
    return Status::ok();
}

void WalFileHeader::encode(std::span<uint8_t, WAL_HEADER_SIZE> dest) const noexcept {
    encode_u32_be(magic, dest.data() + OFFSET_WH_MAGIC);
    encode_u16_be(version, dest.data() + OFFSET_WH_VERSION);
    encode_u16_be(page_size, dest.data() + OFFSET_WH_PAGE_SIZE);
    encode_u64_be(checkpoint_lsn, dest.data() + OFFSET_WH_CHECKPOINT_LSN);
    encode_u64_be(last_lsn, dest.data() + OFFSET_WH_LAST_LSN);
    encode_u32_be(0, dest.data() + OFFSET_WH_CHECKSUM);
    encode_u32_be(0, dest.data() + OFFSET_WH_RESERVED);

    uint32_t crc = calculate_crc32(dest);
    encode_u32_be(crc, dest.data() + OFFSET_WH_CHECKSUM);
}

Result<WalFileHeader> WalFileHeader::decode(std::span<const uint8_t, WAL_HEADER_SIZE> src) noexcept {
    WalFileHeader hdr;
    hdr.magic          = decode_u32_be(src.data() + OFFSET_WH_MAGIC);
    hdr.version        = decode_u16_be(src.data() + OFFSET_WH_VERSION);
    hdr.page_size      = decode_u16_be(src.data() + OFFSET_WH_PAGE_SIZE);
    hdr.checkpoint_lsn = decode_u64_be(src.data() + OFFSET_WH_CHECKPOINT_LSN);
    hdr.last_lsn       = decode_u64_be(src.data() + OFFSET_WH_LAST_LSN);
    hdr.checksum       = decode_u32_be(src.data() + OFFSET_WH_CHECKSUM);

    Status st = hdr.validate();
    if (!st.is_ok()) return st;

    std::array<uint8_t, WAL_HEADER_SIZE> check_buf;
    std::memcpy(check_buf.data(), src.data(), WAL_HEADER_SIZE);
    encode_u32_be(0, check_buf.data() + OFFSET_WH_CHECKSUM);
    uint32_t computed = calculate_crc32(check_buf);
    if (computed != hdr.checksum) {
        return Status::checksum_mismatch("WAL file header checksum mismatch");
    }

    return hdr;
}

WriteAheadLog::WriteAheadLog(std::filesystem::path path) noexcept
    : path_(std::move(path)) {}

WriteAheadLog::~WriteAheadLog() {
    (void)close();
}

WriteAheadLog::WriteAheadLog(WriteAheadLog&& other) noexcept
    : path_(std::move(other.path_)),
      file_(other.file_),
      last_lsn_(other.last_lsn_),
      flushed_lsn_(other.flushed_lsn_),
      write_buffer_(std::move(other.write_buffer_)) {
    other.file_ = nullptr;
    other.last_lsn_ = 0;
    other.flushed_lsn_ = 0;
}

WriteAheadLog& WriteAheadLog::operator=(WriteAheadLog&& other) noexcept {
    if (this != &other) {
        (void)close();
        path_ = std::move(other.path_);
        file_ = other.file_;
        last_lsn_ = other.last_lsn_;
        flushed_lsn_ = other.flushed_lsn_;
        write_buffer_ = std::move(other.write_buffer_);
        other.file_ = nullptr;
        other.last_lsn_ = 0;
        other.flushed_lsn_ = 0;
    }
    return *this;
}

Result<std::unique_ptr<WriteAheadLog>> WriteAheadLog::open_or_create(const std::filesystem::path& path) {
    auto wal = std::make_unique<WriteAheadLog>(path);

    std::error_code ec;
    bool exists = std::filesystem::exists(path, ec);

    if (!exists) {
        wal->file_ = std::fopen(path.string().c_str(), "w+b");
        if (wal->file_ == nullptr) {
            return Status::io_error("Failed to create WAL file: " + path.string());
        }
        Status init_st = wal->write_header(0, 0);
        if (!init_st.is_ok()) {
            return init_st;
        }
        wal->last_lsn_ = 0;
        wal->flushed_lsn_ = 0;
    } else {
        wal->file_ = std::fopen(path.string().c_str(), "r+b");
        if (wal->file_ == nullptr) {
            return Status::io_error("Failed to open existing WAL file: " + path.string());
        }

        std::array<uint8_t, WAL_HEADER_SIZE> hdr_buf;
        if (std::fread(hdr_buf.data(), 1, WAL_HEADER_SIZE, wal->file_) != WAL_HEADER_SIZE) {
            return Status::truncated_file("WAL file header truncated");
        }

        auto hdr_res = WalFileHeader::decode(hdr_buf);
        if (hdr_res.is_err()) {
            return hdr_res.status();
        }

        wal->last_lsn_ = hdr_res.value().last_lsn;
        wal->flushed_lsn_ = hdr_res.value().last_lsn;

        // Scan records to determine actual last LSN and position file at end
        auto recs_res = wal->read_all_records();
        if (recs_res.is_ok() && !recs_res.value().empty()) {
            wal->last_lsn_ = recs_res.value().back().lsn;
            wal->flushed_lsn_ = wal->last_lsn_;
        }

        // Seek to end for future appends
        std::fseek(wal->file_, 0, SEEK_END);
    }

    return wal;
}

Status WriteAheadLog::write_header(lsn_t checkpoint_lsn, lsn_t last_lsn) {
    if (file_ == nullptr) {
        return Status::not_open("WAL file is not open");
    }

    WalFileHeader hdr;
    hdr.checkpoint_lsn = checkpoint_lsn;
    hdr.last_lsn = last_lsn;

    std::array<uint8_t, WAL_HEADER_SIZE> buf;
    hdr.encode(buf);

    std::fseek(file_, 0, SEEK_SET);
    if (std::fwrite(buf.data(), 1, WAL_HEADER_SIZE, file_) != WAL_HEADER_SIZE) {
        return Status::io_error("Failed to write WAL header");
    }

    if (!platform_durable_sync(file_)) {
        return Status::io_error("Failed to sync WAL header to disk");
    }

    // Seek back to end
    std::fseek(file_, 0, SEEK_END);
    return Status::ok();
}

Result<lsn_t> WriteAheadLog::append_record(LogRecord& record) {
    std::lock_guard<std::recursive_mutex> lock(wal_mutex_);
    if (file_ == nullptr) {
        return Status::not_open("WAL file is not open");
    }

    if (record.lsn == INVALID_LSN) {
        record.lsn = ++last_lsn_;
    } else if (record.lsn > last_lsn_) {
        last_lsn_ = record.lsn;
    }

    std::vector<uint8_t> encoded;
    record.encode(encoded);

    if (CrashSimulator::is_enabled() && CrashSimulator::target_point() == CrashPoint::DuringWalWrite) {
        size_t partial = encoded.size() / 2;
        if (partial > 0) {
            (void)std::fwrite(encoded.data(), 1, partial, file_);
            (void)platform_durable_sync(file_);
        }
        CrashSimulator::trigger(CrashPoint::DuringWalWrite);
    }

    size_t written = std::fwrite(encoded.data(), 1, encoded.size(), file_);
    if (written != encoded.size()) {
        return Status::io_error("Failed to write log record to WAL file");
    }

    return record.lsn;
}

Status WriteAheadLog::flush() {
    std::lock_guard<std::recursive_mutex> lock(wal_mutex_);
    if (file_ == nullptr) {
        return Status::not_open("WAL file is not open");
    }

    if (!platform_durable_sync(file_)) {
        return Status::io_error("Failed to durably sync WAL to disk");
    }

    flushed_lsn_ = last_lsn_;
    return Status::ok();
}

Status WriteAheadLog::flush_up_to(lsn_t target_lsn) {
    std::lock_guard<std::recursive_mutex> lock(wal_mutex_);
    if (flushed_lsn_ >= target_lsn) {
        return Status::ok();
    }
    return flush();
}

Result<std::vector<LogRecord>> WriteAheadLog::read_all_records() {
    std::lock_guard<std::recursive_mutex> lock(wal_mutex_);
    if (file_ == nullptr) {
        return Status::not_open("WAL file is not open");
    }

    std::vector<LogRecord> records;

    // Get total file length
    std::fseek(file_, 0, SEEK_END);
    long file_len = std::ftell(file_);
    if (file_len < static_cast<long>(WAL_HEADER_SIZE)) {
        return Status::truncated_file("WAL file is smaller than WAL header");
    }

    std::fseek(file_, WAL_HEADER_SIZE, SEEK_SET);
    long current_pos = WAL_HEADER_SIZE;

    while (current_pos < file_len) {
        long remaining = file_len - current_pos;
        if (remaining < static_cast<long>(LOG_RECORD_HEADER_SIZE)) {
            // Incomplete trailing record header from a crash
            break;
        }

        std::array<uint8_t, LOG_RECORD_HEADER_SIZE> hdr_buf;
        if (std::fread(hdr_buf.data(), 1, LOG_RECORD_HEADER_SIZE, file_) != LOG_RECORD_HEADER_SIZE) {
            break;
        }

        uint32_t rec_magic = decode_u32_be(hdr_buf.data());
        if (rec_magic != WAL_RECORD_MAGIC) {
            // Check if corruption in middle or crashed tail write
            if (current_pos + static_cast<long>(LOG_RECORD_HEADER_SIZE) >= file_len) {
                // Incomplete tail write
                break;
            }
            return Status::wal_corrupted("Corrupted record magic signature at WAL offset " +
                                        std::to_string(current_pos));
        }

        uint32_t b_len = decode_u32_be(hdr_buf.data() + 36);
        uint32_t a_len = decode_u32_be(hdr_buf.data() + 40);

        if (b_len > PAGE_SIZE || a_len > PAGE_SIZE) {
            return Status::wal_corrupted("Corrupted image length at WAL offset " +
                                        std::to_string(current_pos));
        }

        size_t payload_len = static_cast<size_t>(b_len) + static_cast<size_t>(a_len);
        size_t total_record_len = LOG_RECORD_HEADER_SIZE + payload_len;

        if (remaining < static_cast<long>(total_record_len)) {
            // Trailing record was truncated during crash mid-write
            break;
        }

        std::vector<uint8_t> full_record(total_record_len);
        std::memcpy(full_record.data(), hdr_buf.data(), LOG_RECORD_HEADER_SIZE);

        if (payload_len > 0) {
            if (std::fread(full_record.data() + LOG_RECORD_HEADER_SIZE, 1, payload_len, file_) != payload_len) {
                // Failed to read full payload (tail truncation)
                break;
            }
        }

        auto dec_res = LogRecord::decode(full_record);
        if (dec_res.is_err()) {
            if (current_pos + static_cast<long>(total_record_len) >= file_len) {
                // Checksum error at the very end of log is an incomplete tail write
                break;
            }
            return dec_res.status(); // Mid-log corruption is an error!
        }

        records.push_back(std::move(dec_res.value()));
        current_pos += static_cast<long>(total_record_len);
    }

    // Seek back to end for future writes
    std::fseek(file_, 0, SEEK_END);
    return records;
}

Status WriteAheadLog::update_checkpoint_boundary(lsn_t checkpoint_lsn) {
    std::lock_guard<std::recursive_mutex> lock(wal_mutex_);
    if (file_ == nullptr) {
        return Status::not_open("WAL file is not open");
    }
    return write_header(checkpoint_lsn, last_lsn_);
}

Status WriteAheadLog::truncate(lsn_t checkpoint_lsn) {
    return truncate_safe(checkpoint_lsn, INVALID_LSN);
}

Status WriteAheadLog::truncate_safe(lsn_t checkpoint_lsn, lsn_t min_retained_lsn) {
    std::lock_guard<std::recursive_mutex> lock(wal_mutex_);
    if (file_ == nullptr) {
        return Status::not_open("WAL file is not open");
    }

    // Flush any pending memory writes
    Status fl = flush();
    if (!fl.is_ok()) return fl;

    std::vector<LogRecord> retained_records;
    if (min_retained_lsn != INVALID_LSN && min_retained_lsn <= last_lsn_) {
        auto all_recs = read_all_records();
        if (all_recs.is_ok()) {
            for (auto& r : all_recs.value()) {
                if (r.lsn >= min_retained_lsn) {
                    retained_records.push_back(std::move(r));
                }
            }
        }
    }

    std::filesystem::path tmp_path = path_.string() + ".tmp";
    std::FILE* tmp_file = std::fopen(tmp_path.string().c_str(), "w+b");
    if (!tmp_file) {
        return Status::io_error("Failed to create temporary WAL file: " + tmp_path.string());
    }

    // Write header for the new WAL
    WalFileHeader hdr;
    hdr.checkpoint_lsn = checkpoint_lsn;
    hdr.last_lsn = retained_records.empty() ? checkpoint_lsn : last_lsn_;
    std::array<uint8_t, WAL_HEADER_SIZE> buf;
    hdr.encode(buf);
    if (std::fwrite(buf.data(), 1, WAL_HEADER_SIZE, tmp_file) != WAL_HEADER_SIZE) {
        std::fclose(tmp_file);
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        return Status::io_error("Failed to write temporary WAL header");
    }

    // Write retained records if any
    for (const auto& r : retained_records) {
        std::vector<uint8_t> encoded;
        r.encode(encoded);
        if (std::fwrite(encoded.data(), 1, encoded.size(), tmp_file) != encoded.size()) {
            std::fclose(tmp_file);
            std::error_code ec;
            std::filesystem::remove(tmp_path, ec);
            return Status::io_error("Failed to write retained record to temporary WAL");
        }
    }

    if (!platform_durable_sync(tmp_file)) {
        std::fclose(tmp_file);
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        return Status::io_error("Failed to sync temporary WAL file");
    }
    std::fclose(tmp_file);

    // Close the original WAL before atomic replacement
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }

    CrashSimulator::trigger(CrashPoint::DuringWalTruncation);

    // Atomically replace original WAL with temporary WAL
#ifdef _WIN32
    if (!MoveFileExW(tmp_path.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DWORD err = GetLastError();
        return Status::io_error("Failed to replace WAL file atomically: error " + std::to_string(err));
    }
#else
    std::error_code ec;
    std::filesystem::rename(tmp_path, path_, ec);
    if (ec) {
        return Status::io_error("Failed to replace WAL file atomically: " + ec.message());
    }
#endif

    // Reopen newly replaced WAL
    file_ = std::fopen(path_.string().c_str(), "r+b");
    if (!file_) {
        return Status::io_error("Failed to reopen replaced WAL file: " + path_.string());
    }

    std::fseek(file_, 0, SEEK_END);
    last_lsn_ = hdr.last_lsn;
    flushed_lsn_ = hdr.last_lsn;
    return Status::ok();
}

Status WriteAheadLog::close() {
    std::lock_guard<std::recursive_mutex> lock(wal_mutex_);
    if (file_ != nullptr) {
        (void)flush();
        std::fclose(file_);
        file_ = nullptr;
    }
    return Status::ok();
}

size_t WriteAheadLog::file_size() const {
    std::lock_guard<std::recursive_mutex> lock(wal_mutex_);
    if (file_ != nullptr) {
        std::fflush(file_);
        long cur = std::ftell(file_);
        std::fseek(file_, 0, SEEK_END);
        long end = std::ftell(file_);
        std::fseek(file_, cur, SEEK_SET);
        if (end >= 0) return static_cast<size_t>(end);
    }
    std::error_code ec;
    auto sz = std::filesystem::file_size(path_, ec);
    return ec ? 0 : static_cast<size_t>(sz);
}

} // namespace klyrdb
