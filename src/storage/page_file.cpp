#include "klyrdb/storage/page_file.hpp"
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace klyrdb {

PageFile::PageFile(std::filesystem::path path, bool lock_process) noexcept
    : path_(std::move(path)), lock_process_(lock_process) {}

PageFile::~PageFile() {
    (void)close();
}

PageFile::PageFile(PageFile&& other) noexcept
    : path_(std::move(other.path_)),
      lock_process_(other.lock_process_),
      proc_lock_(std::move(other.proc_lock_)),
      file_stream_(std::move(other.file_stream_)),
      metadata_(other.metadata_) {}

PageFile& PageFile::operator=(PageFile&& other) noexcept {
    if (this != &other) {
        (void)close();
        path_ = std::move(other.path_);
        lock_process_ = other.lock_process_;
        proc_lock_ = std::move(other.proc_lock_);
        file_stream_ = std::move(other.file_stream_);
        metadata_ = other.metadata_;
    }
    return *this;
}

Result<std::unique_ptr<PageFile>> PageFile::create(const std::filesystem::path& path, bool lock_process) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return Status::file_already_exists("Database file already exists: " + path.string());
    }

    std::unique_ptr<ProcessFileLock> proc_lock;
    if (lock_process) {
        proc_lock = std::make_unique<ProcessFileLock>(path.string() + ".lock");
        Status lock_st = proc_lock->try_lock(ProcessLockMode::Exclusive);
        if (!lock_st.is_ok()) {
            return lock_st;
        }
    }

    // Ensure parent directory exists if specified
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return Status::io_error("Failed to create parent directory: " + ec.message());
        }
    }

    // 1. Create and truncate file
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            return Status::io_error("Failed to create database file: " + path.string());
        }
    }

    // 2. Open read-write
    auto page_file = std::unique_ptr<PageFile>(new PageFile(path, lock_process));
    page_file->proc_lock_ = std::move(proc_lock);
    page_file->file_stream_.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!page_file->file_stream_.is_open()) {
        return Status::io_error("Failed to open database file in read-write mode: " + path.string());
    }

    // 3. Initialize Page 0 (Database Metadata Page)
    page_file->metadata_.page_count = 1;
    Page page0;
    Status s = page_file->metadata_.encode_to_page(page0);
    if (!s.is_ok()) {
        return s;
    }

    std::array<uint8_t, PAGE_SIZE> page0_buf;
    s = page0.serialize(page0_buf);
    if (!s.is_ok()) {
        return s;
    }

    page_file->file_stream_.seekp(0, std::ios::beg);
    page_file->file_stream_.write(reinterpret_cast<const char*>(page0_buf.data()), PAGE_SIZE);
    if (page_file->file_stream_.fail()) {
        return Status::io_error("Failed to write initial metadata page to " + path.string());
    }

    page_file->file_stream_.flush();
    return page_file;
}

Result<std::unique_ptr<PageFile>> PageFile::open(const std::filesystem::path& path, bool lock_process) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return Status::file_not_found("Database file not found: " + path.string());
    }

    std::unique_ptr<ProcessFileLock> proc_lock;
    if (lock_process) {
        proc_lock = std::make_unique<ProcessFileLock>(path.string() + ".lock");
        Status lock_st = proc_lock->try_lock(ProcessLockMode::Exclusive);
        if (!lock_st.is_ok()) {
            return lock_st;
        }
    }

    if (!std::filesystem::is_regular_file(path, ec)) {
        return Status::io_error("Database path is not a regular file: " + path.string());
    }

    const uintmax_t file_sz = std::filesystem::file_size(path, ec);
    if (ec) {
        return Status::io_error("Failed to determine file size: " + ec.message());
    }

    if (file_sz == 0) {
        return Status::truncated_file("Database file is empty (0 bytes): " + path.string());
    }

    if (file_sz % PAGE_SIZE != 0) {
        return Status::truncated_file("Database file size (" + std::to_string(file_sz) +
                                      " bytes) is not an exact multiple of PAGE_SIZE (" +
                                      std::to_string(PAGE_SIZE) + " bytes)");
    }

    const uint32_t physical_pages = static_cast<uint32_t>(file_sz / PAGE_SIZE);

    auto page_file = std::unique_ptr<PageFile>(new PageFile(path, lock_process));
    page_file->proc_lock_ = std::move(proc_lock);
    page_file->file_stream_.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!page_file->file_stream_.is_open()) {
        return Status::io_error("Failed to open database file in read-write mode: " + path.string());
    }

    // Read and validate Page 0
    std::array<uint8_t, PAGE_SIZE> page0_buf;
    page_file->file_stream_.seekg(0, std::ios::beg);
    page_file->file_stream_.read(reinterpret_cast<char*>(page0_buf.data()), PAGE_SIZE);
    if (page_file->file_stream_.gcount() != static_cast<std::streamsize>(PAGE_SIZE)) {
        return Status::truncated_file("Failed to read full 4096-byte Page 0 from " + path.string());
    }

    Page page0;
    Status s = page0.deserialize(page0_buf);
    if (!s.is_ok()) {
        return s;
    }

    auto meta_res = DatabaseMetadata::decode_from_page(page0);
    if (meta_res.is_err()) {
        return meta_res.status();
    }

    page_file->metadata_ = meta_res.value();
    if (page_file->metadata_.page_count != physical_pages) {
        return Status::page_corrupted("Database metadata page count (" +
                                      std::to_string(page_file->metadata_.page_count) +
                                      ") does not match physical file size (" +
                                      std::to_string(physical_pages) + " pages)");
    }

    return page_file;
}

Status PageFile::read_page(page_id_t page_id, Page& out_page) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!is_open()) {
        return Status::not_open("Database file is not open");
    }

    if (page_id >= metadata_.page_count) {
        return Status::invalid_page_id("Page ID " + std::to_string(page_id) +
                                       " is out of range [0, " +
                                       std::to_string(metadata_.page_count) + ")");
    }

    file_stream_.clear();
    const auto offset = static_cast<std::streamoff>(page_id) * static_cast<std::streamoff>(PAGE_SIZE);
    file_stream_.seekg(offset, std::ios::beg);
    if (file_stream_.fail()) {
        return Status::io_error("Failed to seek to offset for page " + std::to_string(page_id));
    }

    std::array<uint8_t, PAGE_SIZE> buf;
    file_stream_.read(reinterpret_cast<char*>(buf.data()), PAGE_SIZE);
    if (file_stream_.gcount() != static_cast<std::streamsize>(PAGE_SIZE)) {
        return Status::truncated_file("Unexpected end of file while reading page " +
                                      std::to_string(page_id));
    }

    Status s = out_page.deserialize(buf);
    if (!s.is_ok()) {
        return s;
    }

    if (out_page.page_id() != page_id) {
        return Status::page_corrupted("Page at offset " + std::to_string(page_id) +
                                      " contains logical page_id " +
                                      std::to_string(out_page.page_id()));
    }

    return Status::ok();
}

Status PageFile::write_page(const Page& page) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!is_open()) {
        return Status::not_open("Database file is not open");
    }

    const page_id_t pid = page.page_id();
    if (pid >= metadata_.page_count) {
        return Status::invalid_page_id("Cannot write unallocated page ID " +
                                       std::to_string(pid) + " (current count: " +
                                       std::to_string(metadata_.page_count) + ")");
    }

    Page page_copy = page;
    std::array<uint8_t, PAGE_SIZE> buf;
    Status s = page_copy.serialize(buf);
    if (!s.is_ok()) {
        return s;
    }

    file_stream_.clear();
    const auto offset = static_cast<std::streamoff>(pid) * static_cast<std::streamoff>(PAGE_SIZE);
    file_stream_.seekp(offset, std::ios::beg);
    if (file_stream_.fail()) {
        return Status::io_error("Failed to seek to offset for writing page " + std::to_string(pid));
    }

    file_stream_.write(reinterpret_cast<const char*>(buf.data()), PAGE_SIZE);
    if (file_stream_.fail()) {
        return Status::io_error("Failed to write page " + std::to_string(pid) + " to disk");
    }

    return Status::ok();
}

Result<page_id_t> PageFile::append_page(Page& page) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!is_open()) {
        return Status::not_open("Database file is not open");
    }

    const page_id_t new_id = metadata_.page_count;
    page.set_page_id(new_id);

    std::array<uint8_t, PAGE_SIZE> buf;
    Status s = page.serialize(buf);
    if (!s.is_ok()) {
        return s;
    }

    file_stream_.clear();
    const auto offset = static_cast<std::streamoff>(new_id) * static_cast<std::streamoff>(PAGE_SIZE);
    file_stream_.seekp(offset, std::ios::beg);
    if (file_stream_.fail()) {
        return Status::io_error("Failed to seek to end for appending page " + std::to_string(new_id));
    }

    file_stream_.write(reinterpret_cast<const char*>(buf.data()), PAGE_SIZE);
    if (file_stream_.fail()) {
        return Status::io_error("Failed to write appended page " + std::to_string(new_id));
    }

    // Increment page count in metadata and persist Page 0
    metadata_.page_count++;

    Page page0;
    s = metadata_.encode_to_page(page0);
    if (!s.is_ok()) {
        return s;
    }

    std::array<uint8_t, PAGE_SIZE> page0_buf;
    s = page0.serialize(page0_buf);
    if (!s.is_ok()) {
        return s;
    }

    file_stream_.clear();
    file_stream_.seekp(0, std::ios::beg);
    file_stream_.write(reinterpret_cast<const char*>(page0_buf.data()), PAGE_SIZE);
    if (file_stream_.fail()) {
        return Status::io_error("Failed to update Page 0 metadata after append");
    }

    file_stream_.flush();
    return new_id;
}

Status PageFile::update_metadata(const DatabaseMetadata& metadata) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!is_open()) {
        return Status::not_open("Database file is not open");
    }

    const uint32_t current_count = metadata_.page_count;
    metadata_ = metadata;
    if (metadata_.page_count < current_count) {
        metadata_.page_count = current_count;
    }

    Page page0;
    Status s = metadata_.encode_to_page(page0);
    if (!s.is_ok()) {
        return s;
    }

    std::array<uint8_t, PAGE_SIZE> page0_buf;
    s = page0.serialize(page0_buf);
    if (!s.is_ok()) {
        return s;
    }

    file_stream_.clear();
    file_stream_.seekp(0, std::ios::beg);
    file_stream_.write(reinterpret_cast<const char*>(page0_buf.data()), PAGE_SIZE);
    if (file_stream_.fail()) {
        return Status::io_error("Failed to write updated Page 0 metadata");
    }

    file_stream_.flush();
    return Status::ok();
}

Status PageFile::sync() {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!is_open()) {
        return Status::not_open("Database file is not open");
    }
    file_stream_.flush();
    if (file_stream_.fail()) {
        return Status::io_error("Failed to flush database file buffer");
    }

#ifdef _WIN32
    HANDLE h = CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(h);
        CloseHandle(h);
    }
#endif

    return Status::ok();
}

Status PageFile::close() {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (proc_lock_) {
        (void)proc_lock_->unlock();
        proc_lock_.reset();
    }
    if (file_stream_.is_open()) {
        file_stream_.flush();
        file_stream_.close();
        if (file_stream_.fail()) {
            return Status::io_error("Failed to cleanly close database file: " + path_.string());
        }
    }
    return Status::ok();
}

} // namespace klyrdb
