#include "klyrdb/backup/backup_verifier.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/integrity_checker.hpp"
#include <iostream>
#include <iomanip>

namespace klyrdb {

void BackupVerificationReport::print(std::ostream& os) const {
    os << "======================================================================\n";
    os << "                 KLYRDB BACKUP VERIFICATION REPORT                   \n";
    os << "======================================================================\n";
    os << "Status:                " << (is_valid ? "VALID (CLEAN)" : "INVALID (CORRUPT/INCOMPLETE)") << "\n";
    os << "Backup Type:           " << manifest.backup_type << "\n";
    os << "Source Database:       " << manifest.source_db_name << "\n";
    os << "Created At (UTC):      " << manifest.created_at_utc << "\n";
    os << "Format Version:        " << manifest.backup_format_version << " (Storage: " << manifest.storage_format_version << ")\n";
    os << "Pages in Backup:       " << manifest.page_count << " (" << manifest.page_size << " bytes/page)\n";
    os << "Checkpoint LSN:        " << manifest.checkpoint_lsn << "\n";
    os << "Checks Passed:         " << passed_count() << "\n";
    os << "Failures Detected:     " << failure_count() << "\n";
    os << "Warnings:              " << warning_count() << "\n";
    os << "----------------------------------------------------------------------\n";

    if (!checks_passed.empty()) {
        os << "Passed Validations:\n";
        for (const auto& c : checks_passed) {
            os << "  [PASS] " << c << "\n";
        }
    }

    if (!failures.empty()) {
        os << "\nFailures:\n";
        for (const auto& f : failures) {
            os << "  [FAIL] " << f << "\n";
        }
    }

    if (!warnings.empty()) {
        os << "\nWarnings:\n";
        for (const auto& w : warnings) {
            os << "  [WARN] " << w << "\n";
        }
    }

    os << "======================================================================\n";
}

Result<BackupVerificationReport> BackupVerifier::verify(const std::filesystem::path& backup_path) const {
    BackupVerificationReport report;
    report.is_valid = true;

    std::filesystem::path dir = backup_path;
    if (std::filesystem::is_regular_file(backup_path)) {
        if (backup_path.filename() == "manifest.json") {
            dir = backup_path.parent_path();
        } else {
            report.add_failure("Backup path is a regular file, not a backup directory or manifest.json: " + backup_path.string());
            return report;
        }
    }

    if (!std::filesystem::exists(dir)) {
        report.add_failure("Backup directory not found: " + dir.string());
        return report;
    }

    const auto manifest_path = dir / "manifest.json";
    if (!std::filesystem::exists(manifest_path)) {
        report.add_failure("Backup manifest missing: " + manifest_path.string());
        return report;
    }
    report.add_check("Manifest file existence");

    auto m_res = BackupManifest::read_from_file(manifest_path);
    if (m_res.is_err()) {
        report.add_failure("Manifest parsing failed: " + m_res.status().message());
        return report;
    }
    report.manifest = std::move(m_res.value());
    report.add_check("Manifest syntax & JSON structure");

    // 1. Version validation
    if (report.manifest.backup_format_version != BackupManifest::CURRENT_BACKUP_FORMAT_VERSION) {
        report.add_failure("Unsupported backup format version: " +
                           std::to_string(report.manifest.backup_format_version) +
                           " (supported: " + std::to_string(BackupManifest::CURRENT_BACKUP_FORMAT_VERSION) + ")");
    } else {
        report.add_check("Backup format version compatibility");
    }

    if (report.manifest.storage_format_version != BackupManifest::CURRENT_STORAGE_FORMAT_VERSION) {
        report.add_failure("Unsupported storage format version: " +
                           std::to_string(report.manifest.storage_format_version) +
                           " (supported: " + std::to_string(BackupManifest::CURRENT_STORAGE_FORMAT_VERSION) + ")");
    } else {
        report.add_check("Storage format version compatibility");
    }

    // 2. Publication status
    if (report.manifest.status != "Complete") {
        report.add_failure("Backup is incomplete or aborted: status='" + report.manifest.status + "'");
    } else {
        report.add_check("Backup completion status ('Complete')");
    }

    // 3. Database component check
    const auto* db_comp = report.manifest.find_component("database");
    if (db_comp == nullptr) {
        report.add_failure("Manifest does not contain mandatory 'database' component");
        return report;
    }

    const auto db_path = dir / db_comp->filename;
    if (!std::filesystem::exists(db_path)) {
        report.add_failure("Database file not found: " + db_path.string());
        return report;
    }
    report.add_check("Database file existence (" + db_comp->filename + ")");

    std::error_code ec;
    uintmax_t actual_db_size = std::filesystem::file_size(db_path, ec);
    if (ec) {
        report.add_failure("Failed to read database file size: " + ec.message());
    } else if (actual_db_size != db_comp->size_bytes) {
        report.add_failure("Database file size mismatch: expected " + std::to_string(db_comp->size_bytes) +
                           " bytes, actual " + std::to_string(actual_db_size) + " bytes");
    } else {
        report.add_check("Database file size (" + std::to_string(actual_db_size) + " bytes)");
    }

    auto db_crc_res = calculate_file_crc32(db_path);
    if (db_crc_res.is_err()) {
        report.add_failure("Failed to calculate database checksum: " + db_crc_res.status().message());
    } else if (db_crc_res.value() != db_comp->crc32) {
        report.add_failure("Database CRC-32 checksum mismatch: manifest=0x" +
                           [&] { char b[16]; snprintf(b, sizeof(b), "%08X", db_comp->crc32); return std::string(b); }() +
                           ", computed=0x" +
                           [&] { char b[16]; snprintf(b, sizeof(b), "%08X", db_crc_res.value()); return std::string(b); }());
    } else {
        report.add_check("Database CRC-32 bit-level checksum verified");
    }

    // 4. WAL component check if present
    const auto* wal_comp = report.manifest.find_component("wal");
    if (wal_comp != nullptr) {
        const auto wal_path = dir / wal_comp->filename;
        if (!std::filesystem::exists(wal_path)) {
            report.add_failure("WAL file in manifest not found on disk: " + wal_path.string());
        } else {
            uintmax_t actual_wal_size = std::filesystem::file_size(wal_path, ec);
            if (ec) {
                report.add_failure("Failed to read WAL file size: " + ec.message());
            } else if (actual_wal_size != wal_comp->size_bytes) {
                report.add_failure("WAL file size mismatch: expected " + std::to_string(wal_comp->size_bytes) +
                                   ", actual " + std::to_string(actual_wal_size));
            } else {
                report.add_check("WAL file size (" + std::to_string(actual_wal_size) + " bytes)");
            }

            auto wal_crc_res = calculate_file_crc32(wal_path);
            if (wal_crc_res.is_err()) {
                report.add_failure("Failed to calculate WAL checksum: " + wal_crc_res.status().message());
            } else if (wal_crc_res.value() != wal_comp->crc32) {
                report.add_failure("WAL CRC-32 checksum mismatch: manifest=0x" +
                                   [&] { char b[16]; snprintf(b, sizeof(b), "%08X", wal_comp->crc32); return std::string(b); }() +
                                   ", computed=0x" +
                                   [&] { char b[16]; snprintf(b, sizeof(b), "%08X", wal_crc_res.value()); return std::string(b); }());
            } else {
                report.add_check("WAL CRC-32 bit-level checksum verified");
            }
        }
    }

    // 5. Database Geometry & Page 0 Validation
    auto pf_res = PageFile::open(db_path, false);
    if (pf_res.is_err()) {
        report.add_failure("Failed to open database file: " + pf_res.status().message());
        return report;
    }
    report.add_check("Database file format & Page 0 geometry");

    auto& pf = *pf_res.value();
    if (pf.page_count() != report.manifest.page_count) {
        report.add_failure("Manifest page count mismatch: manifest=" +
                           std::to_string(report.manifest.page_count) +
                           ", physical=" + std::to_string(pf.page_count()));
    } else {
        report.add_check("Page count consistency (" + std::to_string(pf.page_count()) + " pages)");
    }

    // 6. Deep Storage Integrity Verification
    if (pf.page_count() > 1) {
        IntegrityChecker checker(pf, nullptr);
        auto int_res = checker.check_all();
        if (int_res.is_err()) {
            report.add_failure("Integrity check execution failed: " + int_res.status().message());
        } else {
            const auto& int_rep = int_res.value();
            if (!int_rep.is_healthy || int_rep.error_count() > 0) {
                for (const auto& iss : int_rep.issues) {
                    if (iss.severity == IntegritySeverity::Error) {
                        report.add_failure("[" + iss.subsystem + "] " + iss.message);
                    } else if (iss.severity == IntegritySeverity::Warning) {
                        report.add_warning("[" + iss.subsystem + "] " + iss.message);
                    }
                }
            } else {
                report.add_check("Full storage & relational integrity verification (CRC, SlottedPages, B+Trees, Constraints)");
            }
        }
    } else {
        report.add_check("Clean empty database metadata verified");
    }

    return report;
}

} // namespace klyrdb
