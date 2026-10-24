#include "klyrdb/backup/logical_importer.hpp"
#include "klyrdb/backup/backup_manifest.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/sql/executor.hpp"
#include "klyrdb/storage/integrity_checker.hpp"
#include "klyrdb/concurrency/process_file_lock.hpp"
#include "klyrdb/concurrency/lock_manager.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <optional>

#ifdef _WIN32
#include <process.h>
#define GETPID _getpid
#else
#include <unistd.h>
#define GETPID getpid
#endif

namespace klyrdb {

namespace {

std::string trim_str(std::string_view sv) {
    size_t first = sv.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return "";
    size_t last = sv.find_last_not_of(" \t\r\n");
    return std::string(sv.substr(first, last - first + 1));
}

class StatementStreamReader {
public:
    explicit StatementStreamReader(std::istream& in) : in_(in) {}

    std::optional<std::string> next_statement() {
        std::string stmt;
        bool in_string = false;
        bool in_comment = false;
        char c;

        while (in_.get(c)) {
            if (in_comment) {
                if (c == '\n') {
                    in_comment = false;
                }
                continue;
            }

            if (in_string) {
                stmt.push_back(c);
                if (c == '\'') {
                    if (in_.peek() == '\'') {
                        char next_c;
                        in_.get(next_c);
                        stmt.push_back(next_c);
                    } else {
                        in_string = false;
                    }
                }
                continue;
            }

            if (c == '-' && in_.peek() == '-') {
                in_comment = true;
                in_.get(); // consume second '-'
                continue;
            }

            if (c == '\'') {
                in_string = true;
                stmt.push_back(c);
                continue;
            }

            if (c == ';') {
                std::string trimmed = trim_str(stmt);
                if (!trimmed.empty()) {
                    return trimmed + ";";
                }
                stmt.clear();
                continue;
            }

            stmt.push_back(c);
        }

        std::string trimmed = trim_str(stmt);
        if (!trimmed.empty()) {
            return trimmed + ";";
        }
        return std::nullopt;
    }

private:
    std::istream& in_;
};

void safe_remove_files(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
    std::filesystem::remove(p.string() + ".wal", ec);
    std::filesystem::remove(p.string() + ".lock", ec);
}

} // namespace

Result<ImportResult> LogicalImporter::import_sql(
    const std::filesystem::path& input_path,
    const std::filesystem::path& db_path,
    const ImportOptions& options) const {

    auto start_time = std::chrono::steady_clock::now();

    // 1. Validate input file exists
    std::error_code ec;
    if (!std::filesystem::exists(input_path, ec)) {
        return Status::file_not_found("Input SQL dump file not found: " + input_path.string());
    }

    std::ifstream dump_file(input_path, std::ios::in | std::ios::binary);
    if (!dump_file.is_open()) {
        return Status::io_error("Failed to open input SQL dump file: " + input_path.string());
    }

    // 2. Validate destination path
    const bool dest_exists = std::filesystem::exists(db_path, ec);
    if (dest_exists && !options.replace_existing) {
        return Status::file_already_exists("Target database file already exists: " + db_path.string() +
                                          " (use replace_existing to overwrite)");
    }

    // Lock check on destination
    ProcessFileLock dest_lock(db_path.string() + ".lock");
    Status lock_st = dest_lock.try_lock(ProcessLockMode::Exclusive);
    if (!lock_st.is_ok()) {
        return Status::database_busy("Target database is currently open or locked by another process");
    }

    // 3. Staged database setup
    const auto parent_dir = db_path.has_parent_path() ? db_path.parent_path() : std::filesystem::current_path();
    std::filesystem::create_directories(parent_dir, ec);

    const auto staged_db_path = parent_dir / (db_path.filename().string() + ".import_tmp." +
                                              std::to_string(GETPID()) + "." + std::to_string(current_epoch_seconds()));
    const auto staged_wal_path = staged_db_path.string() + ".wal";

    safe_remove_files(staged_db_path);

    bool committed = false;
    struct StagingGuard {
        std::filesystem::path p;
        bool& is_committed;
        ~StagingGuard() {
            if (!is_committed) {
                safe_remove_files(p);
            }
        }
    } staging_guard{staged_db_path, committed};

    // Create fresh database at staged path
    auto pf_create_res = PageFile::create(staged_db_path, false);
    if (pf_create_res.is_err()) {
        return pf_create_res.status();
    }
    auto pf = std::move(pf_create_res.value());

    size_t statements_executed = 0;
    size_t tables_imported = 0;
    size_t rows_imported = 0;

    {
        BufferPoolManager bpm(*pf, 64);
        auto wal_res = WriteAheadLog::open_or_create(staged_wal_path);
        if (wal_res.is_err()) {
            return wal_res.status();
        }
        auto& wal = *wal_res.value();
        bpm.set_wal(&wal);
        TransactionManager txn_mgr(bpm, wal);

        auto fsm_res = FreeSpaceMap::open_or_create(bpm);
        auto tc_res = TableCatalog::open_or_create(bpm);
        auto ic_res = IndexCatalog::open_or_create(bpm);

        if (tc_res.is_err()) return tc_res.status();
        if (ic_res.is_err()) return ic_res.status();

        auto& tc = *tc_res.value();
        auto& ic = *ic_res.value();
        RecordManager rm(bpm, fsm_res.is_ok() ? fsm_res.value().get() : nullptr);
        SqlExecutor executor(bpm, rm, tc, ic);
        executor.set_transaction_manager(&txn_mgr);

        // 4. Stream statements and execute
        StatementStreamReader reader(dump_file);
        while (auto stmt_opt = reader.next_statement()) {
            const auto& sql = *stmt_opt;
            auto exec_res = executor.execute(sql);
            if (exec_res.is_err()) {
                if (options.stop_on_error) {
                    if (txn_mgr.current_transaction() != nullptr) {
                        (void)txn_mgr.rollback(txn_mgr.current_transaction());
                    }
                    return Status::import_error("SQL import error on statement #" +
                                                std::to_string(statements_executed + 1) +
                                                ": " + exec_res.status().message() + "\nStatement: " + sql);
                }
            } else {
                statements_executed++;
                const auto& q = exec_res.value();
                if (sql.rfind("CREATE TABLE", 0) == 0 || sql.rfind("create table", 0) == 0) {
                    tables_imported++;
                } else if (sql.rfind("INSERT", 0) == 0 || sql.rfind("insert", 0) == 0) {
                    rows_imported += q.rows_affected() > 0 ? q.rows_affected() : 1;
                }
            }
        }

        // Commit any remaining open transaction
        if (txn_mgr.current_transaction() != nullptr) {
            auto c_st = txn_mgr.commit(txn_mgr.current_transaction());
            if (!c_st.is_ok() && options.stop_on_error) {
                return Status::import_error("Failed to commit final import transaction: " + c_st.message());
            }
        }

        // Flush all pages and sync
        (void)bpm.flush_all_pages();
        (void)pf->sync();
        (void)wal.flush();
    }
    pf.reset();

    // 5. Run post-import integrity check on staging
    {
        auto staged_pf_res = PageFile::open(staged_db_path, false);
        if (staged_pf_res.is_err()) {
            return staged_pf_res.status();
        }
        IntegrityChecker checker(*staged_pf_res.value(), nullptr);
        auto chk_res = checker.check_all();
        if (chk_res.is_err()) {
            return chk_res.status();
        }
        if (!chk_res.value().is_healthy || chk_res.value().error_count() > 0) {
            std::string err = "Imported database failed integrity validation: ";
            for (const auto& iss : chk_res.value().issues) {
                if (iss.severity == IntegritySeverity::Error) {
                    err += "[" + iss.subsystem + "] " + iss.message + "; ";
                }
            }
            return Status::integrity_check_failed(err);
        }
    }

    // 6. Atomic publication to destination
    (void)dest_lock.unlock();

    std::filesystem::path old_db_path;
    if (dest_exists) {
        old_db_path = parent_dir / (db_path.filename().string() + ".old." + std::to_string(GETPID()));
        safe_remove_files(old_db_path);
        std::filesystem::rename(db_path, old_db_path, ec);
        if (ec) {
            return Status::io_error("Failed to move existing target database aside: " + ec.message());
        }
    }

    std::filesystem::rename(staged_db_path, db_path, ec);
    if (ec) {
        if (!old_db_path.empty()) {
            std::error_code roll_ec;
            std::filesystem::rename(old_db_path, db_path, roll_ec);
        }
        return Status::io_error("Failed to atomically publish imported database: " + ec.message());
    }

    // Clean up temporary staged WAL if present
    safe_remove_files(staged_wal_path);
    if (!old_db_path.empty()) safe_remove_files(old_db_path);

    committed = true;

    auto end_time = std::chrono::steady_clock::now();
    ImportResult res;
    res.db_path = db_path;
    res.statements_executed = statements_executed;
    res.tables_imported = tables_imported;
    res.rows_imported = rows_imported;
    res.duration_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    return res;
}

} // namespace klyrdb
