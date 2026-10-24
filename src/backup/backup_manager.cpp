#include "klyrdb/backup/backup_manager.hpp"
#include "klyrdb/backup/backup_verifier.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/recovery/checkpoint_manager.hpp"
#include "klyrdb/concurrency/process_file_lock.hpp"
#include "klyrdb/concurrency/lock_manager.hpp"
#include <chrono>
#include <fstream>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#define GETPID _getpid
#else
#include <unistd.h>
#define GETPID getpid
#endif

namespace klyrdb {

namespace {

void safe_remove_all(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
}

} // namespace

Result<BackupResult> BackupManager::create_backup(
    const std::filesystem::path& source_db_path,
    const std::filesystem::path& backup_path,
    const BackupOptions& options) const {

    auto start_time = std::chrono::steady_clock::now();

    // 1. Validate source path
    std::error_code ec;
    if (!std::filesystem::exists(source_db_path, ec)) {
        return Status::file_not_found("Source database not found: " + source_db_path.string());
    }

    // 2. Validate destination path
    if (std::filesystem::exists(backup_path, ec) && !options.overwrite) {
        return Status::file_already_exists("Backup destination already exists: " + backup_path.string());
    }

    // 3. Acquire process-level lock on the source database
    ProcessFileLock proc_lock(source_db_path.string() + ".lock");
    Status lock_st = proc_lock.lock_with_timeout(ProcessLockMode::Exclusive, options.lock_timeout);
    if (!lock_st.is_ok()) {
        return Status::lock_timeout("Failed to acquire exclusive lock on source database: " + lock_st.message());
    }

    // Acquire in-process lock if one exists in LockManager
    auto in_proc_lock = LockManager::instance().get_lock(source_db_path, false);
    std::optional<ExclusiveLockGuard> excl_guard;
    if (in_proc_lock) {
        auto g_res = ExclusiveLockGuard::acquire(*in_proc_lock, options.lock_timeout);
        if (g_res.is_err()) {
            return g_res.status();
        }
        excl_guard = std::move(g_res.value());
    }

    // 4. Flush dirty pages & establish consistent snapshot via Checkpoint
    lsn_t chk_lsn = 0;
    lsn_t min_req_lsn = 0;
    uint32_t total_pages = 0;
    const std::filesystem::path wal_path = source_db_path.string() + ".wal";
    const bool has_wal = std::filesystem::exists(wal_path, ec);

    {
        auto pf_res = PageFile::open(source_db_path, false);
        if (pf_res.is_err()) {
            return pf_res.status();
        }
        auto& pf = *pf_res.value();
        total_pages = pf.page_count();
        chk_lsn = pf.metadata().checkpoint_lsn;
        min_req_lsn = chk_lsn;

        if (has_wal) {
            auto wal_res = WriteAheadLog::open_or_create(wal_path);
            if (wal_res.is_ok()) {
                auto& wal = *wal_res.value();
                (void)wal.flush();

                // Scan records to find min active txn LSN and latest checkpoint
                auto recs_res = wal.read_all_records();
                if (recs_res.is_ok()) {
                    std::unordered_set<txn_id_t> committed;
                    std::unordered_set<txn_id_t> aborted;
                    std::unordered_map<txn_id_t, lsn_t> txn_first_lsn;
                    for (const auto& rec : recs_res.value()) {
                        if (rec.type == LogRecordType::Checkpoint) {
                            chk_lsn = rec.lsn;
                            min_req_lsn = rec.lsn;
                        } else if (rec.type == LogRecordType::Commit) {
                            committed.insert(rec.txn_id);
                        } else if (rec.type == LogRecordType::Abort) {
                            aborted.insert(rec.txn_id);
                        } else if (rec.type == LogRecordType::Begin ||
                                   rec.type == LogRecordType::PageUpdate ||
                                   rec.type == LogRecordType::PageAlloc) {
                            if (rec.txn_id != 0 && txn_first_lsn.find(rec.txn_id) == txn_first_lsn.end()) {
                                txn_first_lsn[rec.txn_id] = rec.lsn;
                            }
                        }
                    }
                    lsn_t lowest_active = INVALID_LSN;
                    for (const auto& [tid, f_lsn] : txn_first_lsn) {
                        if (committed.find(tid) == committed.end() && aborted.find(tid) == aborted.end()) {
                            if (lowest_active == INVALID_LSN || f_lsn < lowest_active) {
                                lowest_active = f_lsn;
                            }
                        }
                    }
                    if (lowest_active != INVALID_LSN) {
                        min_req_lsn = lowest_active;
                    }
                }
            }
        }
        (void)pf.sync();
    }

    // 5. Create temporary staging directory on the same parent path
    const auto parent_dir = backup_path.has_parent_path() ? backup_path.parent_path() : std::filesystem::current_path();
    std::filesystem::create_directories(parent_dir, ec);

    const auto now_epoch = current_epoch_seconds();
    const auto staging_dir = parent_dir / (backup_path.filename().string() + ".tmp." +
                                           std::to_string(GETPID()) + "." + std::to_string(now_epoch));
    safe_remove_all(staging_dir);
    if (!std::filesystem::create_directories(staging_dir, ec)) {
        return Status::io_error("Failed to create temporary backup staging directory: " + staging_dir.string());
    }

    // Auto-cleanup lambda if operation does not complete successfully
    bool committed = false;
    struct StagingGuard {
        std::filesystem::path path;
        bool& is_committed;
        ~StagingGuard() {
            if (!is_committed) {
                safe_remove_all(path);
            }
        }
    } staging_guard{staging_dir, committed};

    // 6. Copy database file and WAL file to staging using canonical names
    const std::string db_filename = "data.kdb";
    const auto staged_db_path = staging_dir / db_filename;
    std::filesystem::copy_file(source_db_path, staged_db_path, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return Status::io_error("Failed to copy database file to backup staging: " + ec.message());
    }

    std::string wal_filename = "";
    std::filesystem::path staged_wal_path;
    bool wal_copied = false;
    if (has_wal && std::filesystem::exists(wal_path, ec)) {
        wal_filename = "wal.log";
        staged_wal_path = staging_dir / wal_filename;
        std::filesystem::copy_file(wal_path, staged_wal_path, std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            wal_copied = true;
        }
    }

    // 7. Calculate checksums and component sizes
    uint64_t db_size = std::filesystem::file_size(staged_db_path, ec);
    auto db_crc_res = calculate_file_crc32(staged_db_path);
    if (db_crc_res.is_err()) {
        return db_crc_res.status();
    }
    uint32_t db_crc = db_crc_res.value();

    uint64_t wal_size = 0;
    uint32_t wal_crc = 0;
    if (wal_copied) {
        wal_size = std::filesystem::file_size(staged_wal_path, ec);
        auto wal_crc_res = calculate_file_crc32(staged_wal_path);
        if (wal_crc_res.is_ok()) {
            wal_crc = wal_crc_res.value();
        }
    }

    // 8. Generate and write manifest
    BackupManifest manifest;
    manifest.backup_format_version = BackupManifest::CURRENT_BACKUP_FORMAT_VERSION;
    manifest.storage_format_version = BackupManifest::CURRENT_STORAGE_FORMAT_VERSION;
    manifest.backup_type = "PhysicalFull";
    manifest.source_db_name = source_db_path.filename().string();
    manifest.created_at_utc = current_utc_timestamp_iso8601();
    manifest.created_at_epoch = now_epoch;
    manifest.page_size = static_cast<uint32_t>(PAGE_SIZE);
    manifest.page_count = total_pages;
    manifest.checkpoint_lsn = chk_lsn;
    manifest.min_required_lsn = min_req_lsn;
    manifest.status = "Complete";
    manifest.checksum_algo = "CRC32";

    manifest.components.push_back(BackupComponent{
        "database",
        db_filename,
        db_size,
        db_crc
    });

    if (wal_copied) {
        manifest.components.push_back(BackupComponent{
            "wal",
            wal_filename,
            wal_size,
            wal_crc
        });
    }

    const auto manifest_path = staging_dir / "manifest.json";
    Status man_st = manifest.write_to_file(manifest_path);
    if (!man_st.is_ok()) {
        return man_st;
    }

    // 9. Self-verify before publication
    if (options.verify_after_backup) {
        BackupVerifier verifier;
        auto v_res = verifier.verify(staging_dir);
        if (v_res.is_err()) {
            return v_res.status();
        }
        if (!v_res.value().is_valid) {
            std::string err = "Self-verification of staged backup failed: ";
            for (const auto& f : v_res.value().failures) {
                err += f + "; ";
            }
            return Status::corrupt_backup(err);
        }
    }

    // 10. Atomic publication
    std::filesystem::path old_backup_path;
    if (std::filesystem::exists(backup_path, ec) && options.overwrite) {
        old_backup_path = parent_dir / (backup_path.filename().string() + ".old." + std::to_string(GETPID()));
        safe_remove_all(old_backup_path);
        std::filesystem::rename(backup_path, old_backup_path, ec);
        if (ec) {
            return Status::io_error("Failed to move existing backup aside: " + ec.message());
        }
    }

    std::filesystem::rename(staging_dir, backup_path, ec);
    if (ec) {
        if (!old_backup_path.empty()) {
            std::error_code roll_ec;
            std::filesystem::rename(old_backup_path, backup_path, roll_ec);
        }
        return Status::io_error("Failed to atomically publish backup: " + ec.message());
    }

    if (!old_backup_path.empty()) {
        safe_remove_all(old_backup_path);
    }

    committed = true;

    auto end_time = std::chrono::steady_clock::now();
    BackupResult result;
    result.backup_path = backup_path;
    result.manifest = std::move(manifest);
    result.total_backup_bytes = db_size + wal_size;
    result.duration_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    return result;
}

} // namespace klyrdb
