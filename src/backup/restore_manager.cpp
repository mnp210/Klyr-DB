#include "klyrdb/backup/restore_manager.hpp"
#include "klyrdb/backup/backup_verifier.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/recovery/recovery_manager.hpp"
#include "klyrdb/storage/integrity_checker.hpp"
#include "klyrdb/concurrency/process_file_lock.hpp"
#include "klyrdb/concurrency/lock_manager.hpp"
#include <chrono>
#include <fstream>
#include <algorithm>
#include <optional>

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

void safe_remove_file(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

} // namespace

Result<RestoreResult> RestoreManager::restore(
    const std::filesystem::path& backup_path,
    const std::filesystem::path& dest_db_path,
    const RestoreOptions& options) const {

    auto start_time = std::chrono::steady_clock::now();

    // 1. Resolve backup directory
    std::filesystem::path bkp_dir = backup_path;
    if (std::filesystem::is_regular_file(backup_path)) {
        if (backup_path.filename() == "manifest.json") {
            bkp_dir = backup_path.parent_path();
        }
    }

    if (!std::filesystem::exists(bkp_dir)) {
        return Status::file_not_found("Backup source not found: " + bkp_dir.string());
    }

    // 2. Pre-verify backup artifact before touching destination
    BackupManifest manifest;
    if (options.verify_before_restore) {
        BackupVerifier verifier;
        auto v_res = verifier.verify(bkp_dir);
        if (v_res.is_err()) {
            return v_res.status();
        }
        if (!v_res.value().is_valid) {
            std::string err = "Source backup failed pre-restore verification: ";
            for (const auto& f : v_res.value().failures) {
                err += f + "; ";
            }
            return Status::corrupt_backup(err);
        }
        manifest = std::move(v_res.value().manifest);
    } else {
        auto m_res = BackupManifest::read_from_file(bkp_dir / "manifest.json");
        if (m_res.is_err()) return m_res.status();
        manifest = std::move(m_res.value());
    }

    // 3. Destination conflict and lock checks
    std::error_code ec;
    if (std::filesystem::exists(dest_db_path, ec) && !options.force_overwrite) {
        return Status::file_already_exists("Destination database already exists: " + dest_db_path.string() +
                                          " (use force_overwrite to replace)");
    }

    // Check if destination is locked by another process or thread
    ProcessFileLock dest_lock(dest_db_path.string() + ".lock");
    Status lock_st = dest_lock.try_lock(ProcessLockMode::Exclusive);
    if (!lock_st.is_ok()) {
        return Status::lock_timeout("Destination database is currently open or locked by another process: " + lock_st.message());
    }

    auto in_proc_dest_lock = LockManager::instance().get_lock(dest_db_path, false);
    std::optional<ExclusiveLockGuard> dest_excl_guard;
    if (in_proc_dest_lock) {
        auto g_res = ExclusiveLockGuard::acquire(*in_proc_dest_lock, options.lock_timeout);
        if (g_res.is_err()) {
            return g_res.status();
        }
        dest_excl_guard = std::move(g_res.value());
    }

    // 4. Staging files on the destination's directory
    const auto dest_dir = dest_db_path.has_parent_path() ? dest_db_path.parent_path() : std::filesystem::current_path();
    std::filesystem::create_directories(dest_dir, ec);

    const auto staged_db_path = dest_dir / (dest_db_path.filename().string() + ".restore_tmp." +
                                            std::to_string(GETPID()) + "." + std::to_string(current_epoch_seconds()));
    const auto staged_wal_path = std::filesystem::path(staged_db_path.string() + ".wal");

    safe_remove_file(staged_db_path);
    safe_remove_file(staged_wal_path);

    bool committed = false;
    struct StagingGuard {
        std::filesystem::path db_p;
        std::filesystem::path wal_p;
        bool& is_committed;
        ~StagingGuard() {
            if (!is_committed) {
                safe_remove_file(db_p);
                safe_remove_file(wal_p);
            }
        }
    } staging_guard{staged_db_path, staged_wal_path, committed};

    // 5. Copy components to staging
    const auto* db_comp = manifest.find_component("database");
    if (db_comp == nullptr) {
        return Status::corrupt_backup("Backup manifest missing 'database' component");
    }

    const auto src_db_file = bkp_dir / db_comp->filename;
    std::filesystem::copy_file(src_db_file, staged_db_path, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return Status::io_error("Failed to copy database file to restore staging: " + ec.message());
    }

    const auto* wal_comp = manifest.find_component("wal");
    bool wal_restored = false;
    if (wal_comp != nullptr) {
        const auto src_wal_file = bkp_dir / wal_comp->filename;
        if (std::filesystem::exists(src_wal_file, ec)) {
            std::filesystem::copy_file(src_wal_file, staged_wal_path, std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) {
                wal_restored = true;
            }
        }
    }

    // 6. Run crash recovery on staging if WAL is present
    size_t pages_restored = 0;
    if (wal_restored && options.run_recovery) {
        auto pf_res = PageFile::open(staged_db_path, false);
        if (pf_res.is_ok()) {
            auto wal_res = WriteAheadLog::open_or_create(staged_wal_path);
            if (wal_res.is_ok()) {
                BufferPoolManager bpm(*pf_res.value(), 16);
                bpm.set_wal(wal_res.value().get());
                RecoveryManager rec_mgr(bpm, *wal_res.value());
                (void)rec_mgr.recover();
                (void)bpm.flush_all_pages();
                (void)pf_res.value()->sync();
                bpm.set_wal(nullptr);
            }
        }
    }

    // Read restored page count
    {
        auto pf_res = PageFile::open(staged_db_path, false);
        if (pf_res.is_err()) {
            return pf_res.status();
        }
        pages_restored = pf_res.value()->page_count();

        // 7. Post-restore integrity verification
        if (options.verify_after_restore) {
            IntegrityChecker checker(*pf_res.value(), nullptr);
            auto chk_res = checker.check_all();
            if (chk_res.is_err()) {
                return chk_res.status();
            }
            if (!chk_res.value().is_healthy || chk_res.value().error_count() > 0) {
                std::string err = "Restored database failed integrity check: ";
                for (const auto& iss : chk_res.value().issues) {
                    if (iss.severity == IntegritySeverity::Error) {
                        err += "[" + iss.subsystem + "] " + iss.message + "; ";
                    }
                }
                return Status::integrity_check_failed(err);
            }
        }
    }

    // 8. Atomic publication to destination
    // Release in-process and file locks before renaming
    dest_excl_guard.reset();
    (void)dest_lock.unlock();

    std::filesystem::path dest_wal_path = dest_db_path.string() + ".wal";

    std::filesystem::path old_db_path;
    std::filesystem::path old_wal_path;
    if (std::filesystem::exists(dest_db_path, ec)) {
        old_db_path = dest_dir / (dest_db_path.filename().string() + ".old." + std::to_string(GETPID()));
        safe_remove_file(old_db_path);
        std::filesystem::rename(dest_db_path, old_db_path, ec);
        if (ec) {
            return Status::io_error("Failed to move existing destination database aside: " + ec.message());
        }

        if (std::filesystem::exists(dest_wal_path, ec)) {
            old_wal_path = dest_dir / (dest_wal_path.filename().string() + ".old." + std::to_string(GETPID()));
            safe_remove_file(old_wal_path);
            std::filesystem::rename(dest_wal_path, old_wal_path, ec);
        }
    }

    std::filesystem::rename(staged_db_path, dest_db_path, ec);
    if (ec) {
        if (!old_db_path.empty()) {
            std::error_code roll_ec;
            std::filesystem::rename(old_db_path, dest_db_path, roll_ec);
            if (!old_wal_path.empty()) {
                std::filesystem::rename(old_wal_path, dest_wal_path, roll_ec);
            }
        }
        return Status::io_error("Failed to atomically publish restored database: " + ec.message());
    }

    if (wal_restored && std::filesystem::exists(staged_wal_path, ec)) {
        std::filesystem::rename(staged_wal_path, dest_wal_path, ec);
    } else {
        safe_remove_file(dest_wal_path);
    }

    if (!old_db_path.empty()) safe_remove_file(old_db_path);
    if (!old_wal_path.empty()) safe_remove_file(old_wal_path);

    committed = true;

    auto end_time = std::chrono::steady_clock::now();
    RestoreResult result;
    result.dest_db_path = dest_db_path;
    result.pages_restored = pages_restored;
    result.duration_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    result.message = "Successfully restored " + std::to_string(pages_restored) + " pages to " + dest_db_path.string();

    return result;
}

} // namespace klyrdb
