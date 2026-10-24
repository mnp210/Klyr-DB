#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/storage/page.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/wal/write_ahead_log.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/recovery/recovery_manager.hpp"
#include "klyrdb/recovery/checkpoint_manager.hpp"
#include "klyrdb/storage/integrity_checker.hpp"
#include "klyrdb/backup/backup_manifest.hpp"
#include "klyrdb/backup/backup_verifier.hpp"
#include "klyrdb/backup/backup_manager.hpp"
#include "klyrdb/backup/restore_manager.hpp"
#include "klyrdb/backup/logical_exporter.hpp"
#include "klyrdb/backup/logical_importer.hpp"
#include "klyrdb/backup/csv_migrator.hpp"
#include "klyrdb/sql/executor.hpp"
#include "klyrdb/sql/sql_shell.hpp"
#include "klyrdb/diagnostics/metrics.hpp"
#include "klyrdb/catalog/table_statistics.hpp"
#include "klyrdb/version.hpp"
#include <iostream>
#include <iomanip>
#include <string>
#include <string_view>
#include <filesystem>

namespace {

void print_usage(std::string_view program_name) {
    std::cout << "KlyrDB - Relational Database Engine (v" << klyrdb::version().to_string() << ")\n\n"
              << "Usage: " << program_name << " <command> [arguments]\n\n"
              << "Commands:\n"
              << "  sql <file> [query]                       Launch interactive SQL shell or execute a single query\n"
              << "  create <file>                            Create a new empty database file\n"
              << "  info <file>                              Display database metadata, catalogs, tables, and indexes\n"
              << "  read <file> <page_id>                    Read and display a specific page from the database\n"
              << "  recover <file>                           Execute crash recovery from database WAL\n"
              << "  checkpoint <file>                        Execute safe checkpoint and WAL lifecycle management\n"
              << "  integrity-check <file>                   Verify storage, page checksums, and relational integrity\n"
              << "  stats <file>                             Display runtime operational metrics snapshot\n"
              << "  stats-reset <file>                       Reset runtime metrics and cached statistics\n"
              << "  diagnostics <file> [--json]              Generate comprehensive health and metrics report\n"
              << "  analyze <file> [table]                   Gather and cache distribution statistics\n"
              << "  backup <file> <dest_dir> [--overwrite]   Create physical backup snapshot with manifest\n"
              << "  backup-verify <backup_dir>               Verify physical backup integrity without restoring\n"
              << "  restore <backup_dir> <dest_db> [--force] Restore physical backup with verification and recovery\n"
              << "  dump <file> <output.sql>                 Export logical schema DDL and records to SQL\n"
              << "  import <input.sql> <dest_db> [--replace] Import logical SQL dump into database\n"
              << "  export-csv <file> <table> <output.csv>   Export table records to RFC 4180 CSV\n"
              << "  import-csv <input.csv> <file> <table>    Import RFC 4180 CSV into table\n"
              << "  version, --version                       Display engine and format version information\n"
              << "  help, --help                             Display this help message\n";
}

int cmd_version() {
    auto v = klyrdb::version();
    std::cout << "KlyrDB version " << v.to_string() << "\n"
              << "Storage Format Version: " << klyrdb::storage_format_version() << "\n"
              << "Backup Format Version:  " << klyrdb::backup_format_version() << "\n"
              << "Language Standard:      C++20\n";
    return 0;
}

int cmd_sql(const std::string& path, const std::string& optional_query) {
    // Open or create page file
    std::unique_ptr<klyrdb::PageFile> db;
    if (!std::filesystem::exists(path)) {
        auto create_res = klyrdb::PageFile::create(path);
        if (create_res.is_err()) {
            std::cerr << "Error creating database file: " << create_res.status().to_string() << "\n";
            return 1;
        }
        db = std::move(create_res.value());
    } else {
        auto open_res = klyrdb::PageFile::open(path);
        if (open_res.is_err()) {
            std::cerr << "Error opening database file: " << open_res.status().to_string() << "\n";
            return 1;
        }
        db = std::move(open_res.value());
    }

    klyrdb::BufferPoolManager bpm(*db, 64);
    auto metrics = klyrdb::DatabaseMetricsRegistry::instance().get_metrics(path);
    bpm.set_metrics(metrics.get());

    // Open or create Write-Ahead Log
    std::filesystem::path wal_path = path + ".wal";
    auto wal_res = klyrdb::WriteAheadLog::open_or_create(wal_path);
    if (wal_res.is_err()) {
        std::cerr << "Error opening WAL file: " << wal_res.status().to_string() << "\n";
        return 1;
    }
    auto& wal = *wal_res.value();
    bpm.set_wal(&wal);

    // Automatic crash recovery check
    klyrdb::RecoveryManager rec_mgr(bpm, wal);
    if (rec_mgr.needs_recovery()) {
        std::cout << "[*] Pending WAL records detected; running crash recovery...\n";
        auto rec_res = rec_mgr.recover();
        if (rec_res.is_err()) {
            std::cerr << "Recovery failed: " << rec_res.status().to_string() << "\n";
            return 1;
        }
        std::cout << "[+] Recovery completed (Redone: "
                  << rec_res.value().pages_redone << ", Undone: "
                  << rec_res.value().pages_undone << ")\n";
    }

    klyrdb::TransactionManager txn_mgr(bpm, wal);

    auto fsm_res = klyrdb::FreeSpaceMap::open_or_create(bpm);
    auto fsm = fsm_res.is_ok() ? std::move(fsm_res.value()) : nullptr;

    klyrdb::RecordManager rm(bpm, fsm.get());

    auto ic_res = klyrdb::IndexCatalog::open_or_create(bpm);
    if (ic_res.is_err()) {
        std::cerr << "Error opening IndexCatalog: " << ic_res.status().to_string() << "\n";
        return 1;
    }
    auto& ic = *ic_res.value();

    auto tc_res = klyrdb::TableCatalog::open_or_create(bpm);
    if (tc_res.is_err()) {
        std::cerr << "Error opening TableCatalog: " << tc_res.status().to_string() << "\n";
        return 1;
    }
    auto& tc = *tc_res.value();

    klyrdb::SqlExecutor executor(bpm, rm, tc, ic);
    executor.set_transaction_manager(&txn_mgr);

    if (!optional_query.empty()) {
        auto q_res = executor.execute(optional_query);
        if (q_res.is_err()) {
            std::cerr << "Error: " << q_res.status().message() << "\n";
            return 1;
        }
        klyrdb::SqlShell::print_result(q_res.value(), std::cout);
        (void)bpm.flush_all_pages();
        return 0;
    }

    klyrdb::SqlShell shell(executor);
    shell.run();
    (void)bpm.flush_all_pages();
    return 0;
}

int cmd_create(const std::string& path) {
    auto res = klyrdb::PageFile::create(path);
    if (res.is_err()) {
        std::cerr << "Error: " << res.status().to_string() << "\n";
        return 1;
    }
    std::cout << "Successfully created database file: " << path << " (Page 0 initialized)\n";
    return 0;
}

int cmd_info(const std::string& path) {
    auto res = klyrdb::PageFile::open(path);
    if (res.is_err()) {
        std::cerr << "Error opening database: " << res.status().to_string() << "\n";
        return 1;
    }

    auto& db = res.value();
    const auto& meta = db->metadata();

    std::cout << "Database Info: " << path << "\n";
    std::cout << "----------------------------------------\n";
    std::cout << "  Magic:          0x" << std::hex << meta.db_magic << std::dec << " (KDB1)\n";
    std::cout << "  Format Version: " << meta.db_version << "\n";
    std::cout << "  Page Size:      " << meta.page_size << " bytes\n";
    std::cout << "  Total Pages:    " << meta.page_count << "\n";
    std::cout << "  Free List Head: " << (meta.free_list_head == klyrdb::INVALID_PAGE_ID ? "None" : std::to_string(meta.free_list_head)) << "\n";
    std::cout << "  FSM Page:       " << (meta.fsm_page_id == klyrdb::INVALID_PAGE_ID ? "None" : std::to_string(meta.fsm_page_id)) << "\n";
    std::cout << "  Catalog Page:   " << (meta.catalog_page_id == klyrdb::INVALID_PAGE_ID ? "None" : std::to_string(meta.catalog_page_id)) << "\n";
    std::cout << "  Table Catalog:  " << (meta.table_catalog_page_id == klyrdb::INVALID_PAGE_ID ? "None" : std::to_string(meta.table_catalog_page_id)) << "\n";
    std::cout << "  User Version:   " << meta.user_version << "\n";
    std::cout << "  Created (Unix): " << meta.created_at << "\n";

    klyrdb::BufferPoolManager bpm(*db, 16);

    if (meta.table_catalog_page_id != klyrdb::INVALID_PAGE_ID) {
        auto tc_res = klyrdb::TableCatalog::open_or_create(bpm);
        if (tc_res.is_ok()) {
            auto tables = tc_res.value()->list_tables();
            std::cout << "\nRegistered Tables (" << tables.size() << "):\n";
            for (const auto& tbl : tables) {
                std::cout << "  - " << tbl.name << " [ID: " << tbl.table_id << "] (";
                for (size_t i = 0; i < tbl.columns.size(); ++i) {
                    std::cout << tbl.columns[i].name << " " << to_string(tbl.columns[i].type);
                    if (i + 1 < tbl.columns.size()) std::cout << ", ";
                }
                std::cout << ")\n";
                for (const auto& idx : tbl.indexes) {
                    std::cout << "      * Index: " << idx.index_name << " on " << idx.column_name
                              << " (root: " << idx.root_page_id << ")\n";
                }
            }
        }
    }

    if (meta.catalog_page_id != klyrdb::INVALID_PAGE_ID) {
        auto cat_res = klyrdb::IndexCatalog::open_or_create(bpm);
        if (cat_res.is_ok()) {
            auto indexes = cat_res.value()->list_indexes();
            if (indexes.is_ok() && !indexes.value().empty()) {
                std::cout << "\nRegistered B+ Tree Indexes:\n";
                for (const auto& idx : indexes.value()) {
                    std::cout << "  - Index: " << idx.name << " | Root Page: " << idx.root_page_id
                              << " | Key: " << (idx.key_type == klyrdb::KeyType::Int64 ? "Int64" : "String")
                              << (idx.is_unique ? " [UNIQUE]" : "") << "\n";
                }
            }
        }
    }
    return 0;
}

int cmd_read(const std::string& path, uint32_t page_id) {
    auto res = klyrdb::PageFile::open(path);
    if (res.is_err()) {
        std::cerr << "Error opening database: " << res.status().to_string() << "\n";
        return 1;
    }

    klyrdb::Page page;
    auto read_status = res.value()->read_page(page_id, page);
    if (!read_status.is_ok()) {
        std::cerr << "Error reading page " << page_id << ": " << read_status.to_string() << "\n";
        return 1;
    }

    std::cout << "Page " << page_id << " Header:\n";
    std::cout << "----------------------------------------\n";
    std::cout << "  Magic:        0x" << std::hex << page.header().magic << std::dec << "\n";
    std::cout << "  Version:      " << page.header().version << "\n";
    std::cout << "  Type:         " << klyrdb::page_type_to_string(page.page_type()) << " (0x"
              << std::hex << static_cast<uint16_t>(page.page_type()) << std::dec << ")\n";
    std::cout << "  Logical ID:   " << page.page_id() << "\n";
    std::cout << "  Payload Size: " << page.payload_size() << " bytes\n";
    std::cout << "  Checksum:     0x" << std::hex << page.checksum() << std::dec << " (CRC-32)\n";

    auto payload = page.payload();
    if (!payload.empty()) {
        std::cout << "\nPayload Preview (first " << std::min<size_t>(payload.size(), 64) << " bytes):\n";
        for (size_t i = 0; i < std::min<size_t>(payload.size(), 64); ++i) {
            char c = static_cast<char>(payload[i]);
            if (c >= 32 && c <= 126) {
                std::cout << c;
            } else {
                std::cout << '.';
            }
        }
        std::cout << "\n";
    }
    return 0;
}

int cmd_recover(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        std::cerr << "Error: Database file does not exist: " << path << "\n";
        return 1;
    }
    auto open_res = klyrdb::PageFile::open(path);
    if (open_res.is_err()) {
        std::cerr << "Error opening database: " << open_res.status().to_string() << "\n";
        return 1;
    }
    auto db = std::move(open_res.value());
    klyrdb::BufferPoolManager bpm(*db, 64);
    auto metrics = klyrdb::DatabaseMetricsRegistry::instance().get_metrics(path);
    bpm.set_metrics(metrics.get());

    std::filesystem::path wal_path = path + ".wal";
    auto wal_res = klyrdb::WriteAheadLog::open_or_create(wal_path);
    if (wal_res.is_err()) {
        std::cerr << "Error opening WAL: " << wal_res.status().to_string() << "\n";
        return 1;
    }
    auto& wal = *wal_res.value();
    bpm.set_wal(&wal);

    klyrdb::RecoveryManager rec_mgr(bpm, wal);
    if (!rec_mgr.needs_recovery()) {
        std::cout << "[+] Database is already consistent. No recovery required.\n";
        return 0;
    }

    std::cout << "[*] Executing recovery from " << wal_path.string() << "...\n";
    auto stats_res = rec_mgr.recover();
    if (stats_res.is_err()) {
        std::cerr << "Recovery failed: " << stats_res.status().to_string() << "\n";
        return 1;
    }
    const auto& s = stats_res.value();
    std::cout << "[+] Recovery completed successfully:\n"
              << "    Total Log Records: " << s.total_log_records << "\n"
              << "    Committed Txns:    " << s.committed_txns << "\n"
              << "    Uncommitted Txns:  " << s.uncommitted_txns << "\n"
              << "    Pages Redone:      " << s.pages_redone << "\n"
              << "    Pages Undone:      " << s.pages_undone << "\n";
    return 0;
}

int cmd_checkpoint(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        std::cerr << "Error: Database file does not exist: " << path << "\n";
        return 1;
    }

    auto open_res = klyrdb::PageFile::open(path, true);
    if (open_res.is_err()) {
        std::cerr << "Error opening database file: " << open_res.status().to_string() << "\n";
        return 1;
    }
    auto& db = *open_res.value();
    klyrdb::BufferPoolManager bpm(db, 64);

    std::filesystem::path wal_path = path + ".wal";
    auto wal_res = klyrdb::WriteAheadLog::open_or_create(wal_path);
    if (wal_res.is_err()) {
        std::cerr << "Error opening WAL: " << wal_res.status().to_string() << "\n";
        return 1;
    }
    auto& wal = *wal_res.value();
    bpm.set_wal(&wal);

    klyrdb::TransactionManager txn_mgr(bpm, wal);
    klyrdb::CheckpointManager chk_mgr(bpm, wal, txn_mgr);

    auto chk_res = chk_mgr.checkpoint(klyrdb::CheckpointMode::Safe);
    if (chk_res.is_err()) {
        std::cerr << "Checkpoint failed: " << chk_res.status().to_string() << "\n";
        return 1;
    }

    const auto& s = chk_res.value();
    std::cout << "[+] Checkpoint completed successfully in " << std::fixed << std::setprecision(2) << s.duration_ms << " ms\n"
              << "    Checkpoint LSN:     " << s.checkpoint_lsn << "\n"
              << "    Min Required LSN:   " << s.min_required_lsn << "\n"
              << "    Active Txns:        " << s.active_txns_count << "\n"
              << "    WAL Size Before:    " << s.wal_bytes_before << " bytes\n"
              << "    WAL Size After:     " << s.wal_bytes_after << " bytes\n";
    return 0;
}

int cmd_integrity_check(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        std::cerr << "Error: Database file does not exist: " << path << "\n";
        return 1;
    }

    auto open_res = klyrdb::PageFile::open(path, false);
    if (open_res.is_err()) {
        std::cerr << "Error opening database file: " << open_res.status().to_string() << "\n";
        return 1;
    }
    auto& db = *open_res.value();

    std::filesystem::path wal_path = path + ".wal";
    std::unique_ptr<klyrdb::WriteAheadLog> wal;
    if (std::filesystem::exists(wal_path)) {
        auto wal_res = klyrdb::WriteAheadLog::open_or_create(wal_path);
        if (wal_res.is_ok()) {
            wal = std::move(wal_res.value());
        }
    }

    klyrdb::IntegrityChecker checker(db, wal.get());
    auto rep_res = checker.check_all();
    if (rep_res.is_err()) {
        std::cerr << "Integrity check failed: " << rep_res.status().to_string() << "\n";
        return 1;
    }

    const auto& report = rep_res.value();
    report.print(std::cout);
    return report.is_healthy ? 0 : 2;
}

int cmd_backup(const std::string& db_path, const std::string& backup_path, bool overwrite) {
    klyrdb::BackupManager mgr;
    klyrdb::BackupOptions options;
    options.overwrite = overwrite;
    auto res = mgr.create_backup(db_path, backup_path, options);
    if (res.is_err()) {
        std::cerr << "Backup failed: " << res.status().to_string() << "\n";
        return 1;
    }
    const auto& val = res.value();
    std::cout << "[+] Backup completed successfully in " << std::fixed << std::setprecision(2)
              << val.duration_ms << " ms\n"
              << "    Destination:     " << val.backup_path.string() << "\n"
              << "    Database Pages:  " << val.manifest.page_count << "\n";
    if (const auto* db_comp = val.manifest.find_component("database")) {
        std::cout << "    Database Bytes:  " << db_comp->size_bytes << " (CRC32: 0x"
                  << std::hex << std::uppercase << db_comp->crc32 << std::dec << ")\n";
    }
    if (const auto* wal_comp = val.manifest.find_component("wal")) {
        std::cout << "    WAL Bytes:       " << wal_comp->size_bytes << " (CRC32: 0x"
                  << std::hex << std::uppercase << wal_comp->crc32 << std::dec << ")\n";
    }
    std::cout << "    Total Bytes:     " << val.total_backup_bytes << "\n";
    return 0;
}

int cmd_backup_verify(const std::string& backup_path) {
    klyrdb::BackupVerifier verifier;
    auto res = verifier.verify(backup_path);
    if (res.is_err()) {
        std::cerr << "Backup verification failed: " << res.status().to_string() << "\n";
        return 1;
    }
    const auto& report = res.value();
    report.print(std::cout);
    return report.is_valid ? 0 : 2;
}

int cmd_restore(const std::string& backup_path, const std::string& dest_db, bool force) {
    klyrdb::RestoreManager mgr;
    klyrdb::RestoreOptions options;
    options.force_overwrite = force;
    auto res = mgr.restore(backup_path, dest_db, options);
    if (res.is_err()) {
        std::cerr << "Restore failed: " << res.status().to_string() << "\n";
        return 1;
    }
    const auto& val = res.value();
    std::cout << "[+] Restore completed successfully in " << std::fixed << std::setprecision(2)
              << val.duration_ms << " ms\n"
              << "    Destination:     " << val.dest_db_path.string() << "\n"
              << "    Pages Restored:  " << val.pages_restored << "\n"
              << "    Status Message:  " << val.message << "\n";
    return 0;
}

int cmd_dump(const std::string& db_path, const std::string& output_sql) {
    klyrdb::LogicalExporter exporter;
    auto res = exporter.export_sql(db_path, output_sql);
    if (res.is_err()) {
        std::cerr << "SQL dump failed: " << res.status().to_string() << "\n";
        return 1;
    }
    const auto& val = res.value();
    std::cout << "[+] Logical SQL dump created in " << std::fixed << std::setprecision(2)
              << val.duration_ms << " ms\n"
              << "    Output:          " << val.output_path.string() << "\n"
              << "    Tables Exported: " << val.tables_exported << "\n"
              << "    Rows Exported:   " << val.rows_exported << "\n"
              << "    Indexes Exported:" << val.indexes_exported << "\n";
    return 0;
}

int cmd_import(const std::string& input_sql, const std::string& dest_db, bool replace) {
    klyrdb::LogicalImporter importer;
    klyrdb::ImportOptions options;
    options.replace_existing = replace;
    auto res = importer.import_sql(input_sql, dest_db, options);
    if (res.is_err()) {
        std::cerr << "SQL import failed: " << res.status().to_string() << "\n";
        return 1;
    }
    const auto& val = res.value();
    std::cout << "[+] Logical SQL import completed in " << std::fixed << std::setprecision(2)
              << val.duration_ms << " ms\n"
              << "    Statements:      " << val.statements_executed << "\n"
              << "    Tables Created:  " << val.tables_imported << "\n"
              << "    Rows Inserted:   " << val.rows_imported << "\n";
    return 0;
}

int cmd_export_csv(const std::string& db_path, const std::string& table_name, const std::string& output_csv) {
    klyrdb::CsvMigrator migrator;
    auto res = migrator.export_csv(db_path, table_name, output_csv);
    if (res.is_err()) {
        std::cerr << "CSV export failed: " << res.status().to_string() << "\n";
        return 1;
    }
    const auto& val = res.value();
    std::cout << "[+] Exported " << val.rows_processed << " rows to " << output_csv
              << " in " << std::fixed << std::setprecision(2) << val.duration_ms << " ms\n";
    return 0;
}

int cmd_import_csv(const std::string& input_csv, const std::string& db_path, const std::string& table_name) {
    klyrdb::CsvMigrator migrator;
    auto res = migrator.import_csv(input_csv, db_path, table_name);
    if (res.is_err()) {
        std::cerr << "CSV import failed: " << res.status().to_string() << "\n";
        return 1;
    }
    const auto& val = res.value();
    std::cout << "[+] Imported " << val.rows_processed << " rows into table '" << table_name
              << "' in " << std::fixed << std::setprecision(2) << val.duration_ms << " ms\n";
    return 0;
}

int cmd_stats(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        std::cerr << "Error: Database file does not exist: " << path << "\n";
        return 1;
    }
    auto metrics = klyrdb::DatabaseMetricsRegistry::instance().get_metrics(path);
    std::cout << metrics->snapshot().to_string();
    return 0;
}

int cmd_stats_reset(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        std::cerr << "Error: Database file does not exist: " << path << "\n";
        return 1;
    }
    klyrdb::DatabaseMetricsRegistry::instance().reset_metrics(path);
    klyrdb::TableStatisticsRegistry::instance().clear(path);
    std::cout << "[+] Metrics and statistics reset for: " << path << "\n";
    return 0;
}

int cmd_diagnostics(const std::string& path, bool json_format) {
    if (!std::filesystem::exists(path)) {
        std::cerr << "Error: Database file does not exist: " << path << "\n";
        return 1;
    }

    auto pf_res = klyrdb::PageFile::open(path, true);
    if (pf_res.is_err()) {
        std::cerr << "Error opening database: " << pf_res.status().to_string() << "\n";
        return 1;
    }
    auto& pf = *pf_res.value();

    std::filesystem::path wal_path = path + ".wal";
    std::unique_ptr<klyrdb::WriteAheadLog> wal;
    if (std::filesystem::exists(wal_path)) {
        auto w_res = klyrdb::WriteAheadLog::open_or_create(wal_path);
        if (w_res.is_ok()) {
            wal = std::move(w_res.value());
        }
    }

    klyrdb::IntegrityChecker checker(pf, wal.get());
    auto rep_res = checker.check_all();
    if (rep_res.is_err()) {
        std::cerr << "Integrity verification error: " << rep_res.status().to_string() << "\n";
        return 1;
    }
    const auto& report = rep_res.value();

    auto metrics = klyrdb::DatabaseMetricsRegistry::instance().get_metrics(path);
    auto snapshot = metrics->snapshot();

    auto table_stats = klyrdb::TableStatisticsRegistry::instance().get_all_statistics(path);

    if (json_format) {
        std::cout << "{\"database\":\"" << path << "\""
                  << ",\"total_pages\":" << pf.page_count()
                  << ",\"healthy\":" << (report.is_healthy ? "true" : "false")
                  << ",\"tables_checked\":" << report.tables_checked
                  << ",\"records_checked\":" << report.records_checked
                  << ",\"indexes_checked\":" << report.indexes_checked
                  << ",\"error_count\":" << report.error_count()
                  << ",\"metrics\":" << snapshot.to_json()
                  << ",\"statistics\":[";
        for (size_t i = 0; i < table_stats.size(); ++i) {
            if (i > 0) std::cout << ",";
            std::cout << table_stats[i].to_json();
        }
        std::cout << "]}\n";
    } else {
        std::cout << "============================================================\n"
                  << "KlyrDB Runtime Diagnostics & Health Report\n"
                  << "============================================================\n"
                  << "Database:         " << path << "\n"
                  << "Total Pages:      " << pf.page_count() << " (" << (pf.page_count() * klyrdb::PAGE_SIZE / 1024) << " KB)\n"
                  << "Integrity Status: " << (report.is_healthy ? "HEALTHY" : "CORRUPT") << "\n"
                  << "Tables Checked:   " << report.tables_checked << "\n"
                  << "Records Checked:  " << report.records_checked << "\n"
                  << "Indexes Checked:  " << report.indexes_checked << "\n"
                  << "Integrity Errors: " << report.error_count() << "\n\n";

        if (!report.is_healthy) {
            std::cout << "Corruption Details:\n";
            for (const auto& issue : report.issues) {
                std::cout << "  - [" << issue.subsystem << "] " << issue.message << "\n";
            }
            std::cout << "\n";
        }

        std::cout << snapshot.to_string();

        if (!table_stats.empty()) {
            std::cout << "\nTable & Index Statistics:\n";
            for (const auto& ts : table_stats) {
                std::cout << ts.to_string();
            }
        }
    }

    return report.is_healthy ? 0 : 2;
}

int cmd_analyze(const std::string& path, const std::string& table_name) {
    if (!std::filesystem::exists(path)) {
        std::cerr << "Error: Database file does not exist: " << path << "\n";
        return 1;
    }

    auto pf_res = klyrdb::PageFile::open(path, false);
    if (pf_res.is_err()) {
        std::cerr << "Error opening database: " << pf_res.status().to_string() << "\n";
        return 1;
    }

    klyrdb::BufferPoolManager bpm(*pf_res.value(), 64);
    auto tc_res = klyrdb::TableCatalog::open_or_create(bpm);
    if (tc_res.is_err()) {
        std::cerr << "Error opening TableCatalog: " << tc_res.status().to_string() << "\n";
        return 1;
    }
    auto& tc = *tc_res.value();

    auto fsm_res = klyrdb::FreeSpaceMap::open_or_create(bpm);
    auto fsm = fsm_res.is_ok() ? std::move(fsm_res.value()) : nullptr;
    klyrdb::RecordManager rm(bpm, fsm.get());

    if (table_name.empty()) {
        auto res = klyrdb::analyze_database(bpm, rm, tc);
        if (res.is_err()) {
            std::cerr << "Error during analyze: " << res.status().to_string() << "\n";
            return 1;
        }
        std::cout << "[+] Analyzed " << res.value().size() << " tables:\n\n";
        for (const auto& ts : res.value()) {
            std::cout << ts.to_string() << "\n";
        }
    } else {
        auto tbl_res = tc.get_table(table_name);
        if (tbl_res.is_err()) {
            std::cerr << "Error: " << tbl_res.status().to_string() << "\n";
            return 1;
        }
        auto res = klyrdb::analyze_table(bpm, rm, tbl_res.value());
        if (res.is_err()) {
            std::cerr << "Error during analyze: " << res.status().to_string() << "\n";
            return 1;
        }
        std::cout << res.value().to_string() << "\n";
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const std::string_view command = argv[1];

    if (command == "help" || command == "--help" || command == "-h") {
        print_usage(argv[0]);
        return 0;
    } else if (command == "version" || command == "--version" || command == "-v") {
        return cmd_version();
    } else if (command == "sql") {
        if (argc < 3) {
            std::cerr << "Error: Missing database file for sql command\n";
            print_usage(argv[0]);
            return 1;
        }
        std::string query = (argc >= 4) ? argv[3] : "";
        return cmd_sql(argv[2], query);
    } else if (command == "create") {
        if (argc < 3) {
            std::cerr << "Error: Missing file path for create command\n";
            print_usage(argv[0]);
            return 1;
        }
        return cmd_create(argv[2]);
    } else if (command == "recover") {
        if (argc < 3) {
            std::cerr << "Error: Missing file path for recover command\n";
            print_usage(argv[0]);
            return 1;
        }
        return cmd_recover(argv[2]);
    } else if (command == "checkpoint") {
        if (argc < 3) {
            std::cerr << "Error: Missing file path for checkpoint command\n";
            print_usage(argv[0]);
            return 1;
        }
        return cmd_checkpoint(argv[2]);
    } else if (command == "integrity-check") {
        if (argc < 3) {
            std::cerr << "Error: Missing file path for integrity-check command\n";
            print_usage(argv[0]);
            return 1;
        }
        return cmd_integrity_check(argv[2]);
    } else if (command == "stats") {
        if (argc < 3) {
            std::cerr << "Error: Missing file path for stats command\n";
            print_usage(argv[0]);
            return 1;
        }
        return cmd_stats(argv[2]);
    } else if (command == "stats-reset") {
        if (argc < 3) {
            std::cerr << "Error: Missing file path for stats-reset command\n";
            print_usage(argv[0]);
            return 1;
        }
        return cmd_stats_reset(argv[2]);
    } else if (command == "diagnostics") {
        if (argc < 3) {
            std::cerr << "Error: Missing file path for diagnostics command\n";
            print_usage(argv[0]);
            return 1;
        }
        bool json = (argc >= 4 && std::string_view(argv[3]) == "--json");
        return cmd_diagnostics(argv[2], json);
    } else if (command == "analyze") {
        if (argc < 3) {
            std::cerr << "Error: Missing file path for analyze command\n";
            print_usage(argv[0]);
            return 1;
        }
        std::string table = (argc >= 4) ? argv[3] : "";
        return cmd_analyze(argv[2], table);
    } else if (command == "backup") {
        if (argc < 4) {
            std::cerr << "Error: Missing arguments for backup (expected <file> <dest_dir> [--overwrite])\n";
            print_usage(argv[0]);
            return 1;
        }
        bool overwrite = (argc >= 5 && std::string_view(argv[4]) == "--overwrite");
        return cmd_backup(argv[2], argv[3], overwrite);
    } else if (command == "backup-verify") {
        if (argc < 3) {
            std::cerr << "Error: Missing backup directory for backup-verify\n";
            print_usage(argv[0]);
            return 1;
        }
        return cmd_backup_verify(argv[2]);
    } else if (command == "restore") {
        if (argc < 4) {
            std::cerr << "Error: Missing arguments for restore (expected <backup_dir> <dest_db> [--force])\n";
            print_usage(argv[0]);
            return 1;
        }
        bool force = (argc >= 5 && std::string_view(argv[4]) == "--force");
        return cmd_restore(argv[2], argv[3], force);
    } else if (command == "dump") {
        if (argc < 4) {
            std::cerr << "Error: Missing arguments for dump (expected <file> <output.sql>)\n";
            print_usage(argv[0]);
            return 1;
        }
        return cmd_dump(argv[2], argv[3]);
    } else if (command == "import") {
        if (argc < 4) {
            std::cerr << "Error: Missing arguments for import (expected <input.sql> <dest_db> [--replace])\n";
            print_usage(argv[0]);
            return 1;
        }
        bool replace = (argc >= 5 && std::string_view(argv[4]) == "--replace");
        return cmd_import(argv[2], argv[3], replace);
    } else if (command == "export-csv") {
        if (argc < 5) {
            std::cerr << "Error: Missing arguments for export-csv (expected <file> <table> <output.csv>)\n";
            print_usage(argv[0]);
            return 1;
        }
        return cmd_export_csv(argv[2], argv[3], argv[4]);
    } else if (command == "import-csv") {
        if (argc < 5) {
            std::cerr << "Error: Missing arguments for import-csv (expected <input.csv> <file> <table>)\n";
            print_usage(argv[0]);
            return 1;
        }
        return cmd_import_csv(argv[2], argv[3], argv[4]);
    } else if (command == "info") {
        if (argc < 3) {
            std::cerr << "Error: Missing file path for info command\n";
            print_usage(argv[0]);
            return 1;
        }
        return cmd_info(argv[2]);
    } else if (command == "read") {
        if (argc < 4) {
            std::cerr << "Error: Missing arguments for read command (expected <file> <page_id>)\n";
            print_usage(argv[0]);
            return 1;
        }
        try {
            uint32_t page_id = static_cast<uint32_t>(std::stoul(argv[3]));
            return cmd_read(argv[2], page_id);
        } catch (const std::exception&) {
            std::cerr << "Error: Invalid page ID: " << argv[3] << "\n";
            return 1;
        }
    } else {
        std::cerr << "Unknown command: " << command << "\n\n";
        print_usage(argv[0]);
        return 1;
    }
}
