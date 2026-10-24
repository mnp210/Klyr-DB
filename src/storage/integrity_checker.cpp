#include "klyrdb/storage/integrity_checker.hpp"
#include "klyrdb/storage/metadata_page.hpp"
#include "klyrdb/storage/slotted_page.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/index/bplus_tree.hpp"
#include "klyrdb/index/bplus_tree_node.hpp"
#include "klyrdb/sql/row.hpp"
#include "klyrdb/concurrency/lock_manager.hpp"
#include "klyrdb/common/checksum.hpp"
#include "klyrdb/common/endian.hpp"

#include <iostream>
#include <iomanip>
#include <unordered_set>
#include <set>
#include <map>

namespace klyrdb {

void IntegrityReport::print(std::ostream& os) const {
    os << "======================================================================\n";
    os << "                 KLYRDB STORAGE INTEGRITY REPORT                      \n";
    os << "======================================================================\n";
    os << "Status:                " << (is_healthy ? "HEALTHY (CLEAN)" : "CORRUPTED / DEFECTS FOUND") << "\n";
    os << "Total Pages Checked:   " << total_pages_checked << "\n";
    os << "Corrupted Pages:       " << pages_corrupted << "\n";
    os << "Tables Checked:        " << tables_checked << "\n";
    os << "Indexes Checked:       " << indexes_checked << "\n";
    os << "Records Audited:       " << records_checked << "\n";
    os << "FSM Pages Audited:     " << fsm_pages_checked << "\n";
    os << "WAL Checked:           " << (wal_checked ? (wal_healthy ? "VALID" : "CORRUPTED") : "NOT PRESENT / SKIPPED") << "\n";
    os << "Total Errors:          " << error_count() << "\n";
    os << "Total Warnings:        " << warning_count() << "\n";
    os << "----------------------------------------------------------------------\n";

    if (issues.empty()) {
        os << "All integrity checks passed with 0 errors and 0 warnings.\n";
    } else {
        os << "Issues Detected (" << issues.size() << "):\n";
        for (size_t i = 0; i < issues.size(); ++i) {
            const auto& issue = issues[i];
            os << "  [" << std::setw(2) << (i + 1) << "] "
               << "[" << to_string(issue.severity) << "] "
               << "[" << issue.subsystem << "] ";
            if (issue.page_id != INVALID_PAGE_ID) {
                os << "Page " << issue.page_id << " ";
            }
            if (!issue.object_name.empty()) {
                os << "('" << issue.object_name << "') ";
            }
            os << ": " << issue.message << "\n";
        }
    }
    os << "======================================================================\n";
}

IntegrityChecker::IntegrityChecker(PageFile& page_file, WriteAheadLog* wal) noexcept
    : page_file_(page_file),
      wal_(wal),
      db_lock_(LockManager::instance().get_lock(page_file.path(), false)) {}

Result<IntegrityReport> IntegrityChecker::check_all() {
    // Acquire shared lock on database to ensure no concurrent writes occur during check
    std::optional<SharedLockGuard> shared_guard;
    if (db_lock_) {
        auto g_res = SharedLockGuard::acquire(*db_lock_);
        if (g_res.is_err()) {
            return g_res.status();
        }
        shared_guard = std::move(g_res.value());
    }

    IntegrityReport report;

    check_header(report);
    check_pages_and_checksums(report);
    check_slotted_pages(report);
    check_catalogs_and_indexes(report);
    check_foreign_keys(report);
    check_free_space_map(report);
    check_wal(report);

    report.is_healthy = (report.error_count() == 0);
    return report;
}

void IntegrityChecker::check_header(IntegrityReport& report) {
    if (page_file_.page_count() == 0) {
        report.add_error("Header", "PageFile has 0 pages (empty file)");
        return;
    }

    Page p0;
    auto read_st = page_file_.read_page(0, p0);
    if (!read_st.is_ok()) {
        report.add_error("Header", "Failed to read Page 0: " + read_st.message(), 0);
        return;
    }

    if (p0.page_type() != PageType::Metadata) {
        report.add_error("Header", "Page 0 type is not Metadata (got type " +
                         std::to_string(static_cast<uint16_t>(p0.page_type())) + ")", 0);
    }

    auto meta_res = DatabaseMetadata::decode_from_page(p0);
    if (meta_res.is_err()) {
        report.add_error("Header", "Failed to decode DatabaseMetadata: " + meta_res.status().message(), 0);
        return;
    }

    const auto& meta = meta_res.value();
    if (meta.page_count != page_file_.page_count()) {
        report.add_error("Header", "Metadata page count (" + std::to_string(meta.page_count) +
                         ") does not match physical page count (" +
                         std::to_string(page_file_.page_count()) + ")", 0);
    }
}

void IntegrityChecker::check_pages_and_checksums(IntegrityReport& report) {
    size_t count = page_file_.page_count();
    report.total_pages_checked = count;

    for (page_id_t pid = 0; pid < count; ++pid) {
        Page page;
        auto read_st = page_file_.read_page(pid, page);
        if (!read_st.is_ok()) {
            report.add_error("Page", "Failed to read page: " + read_st.message(), pid);
            report.pages_corrupted++;
            continue;
        }

        // Validate page header ID
        if (page.page_id() != pid) {
            report.add_error("Page", "Header page_id mismatch: expected " + std::to_string(pid) +
                             ", found " + std::to_string(page.page_id()), pid);
            report.pages_corrupted++;
        }

        // Validate page type against defined PageType enum
        switch (page.page_type()) {
            case PageType::Metadata:
            case PageType::Data:
            case PageType::Record:
            case PageType::FreeSpaceMap:
            case PageType::IndexCatalog:
            case PageType::TableCatalog:
            case PageType::BTreeLeaf:
            case PageType::BTreeInternal:
            case PageType::Free:
                break;
            default:
                report.add_error("Page", "Unknown or invalid PageType " +
                                 std::to_string(static_cast<uint16_t>(page.page_type())), pid);
                report.pages_corrupted++;
                break;
        }

        // Validate page checksum
        uint32_t stored_crc = page.checksum();
        std::array<uint8_t, PAGE_SIZE> raw_copy;
        std::memcpy(raw_copy.data(), page.raw_data().data(), PAGE_SIZE);
        encode_u32_be(0, raw_copy.data() + OFFSET_PAGE_CHECKSUM);
        uint32_t computed_crc = calculate_crc32(raw_copy);

        if (stored_crc != 0 && stored_crc != computed_crc) {
            report.add_error("Page", "Page checksum mismatch (stored: 0x" +
                             std::to_string(stored_crc) + ", computed: 0x" +
                             std::to_string(computed_crc) + ")", pid);
            report.pages_corrupted++;
        }
    }
}

void IntegrityChecker::check_slotted_pages(IntegrityReport& report) {
    size_t count = page_file_.page_count();

    for (page_id_t pid = 0; pid < count; ++pid) {
        Page page;
        if (!page_file_.read_page(pid, page).is_ok()) continue;

        if (page.page_type() != PageType::Record && page.page_type() != PageType::Data) {
            continue;
        }

        auto payload = page.payload();
        if (payload.size() < SLOTTED_PAGE_HEADER_SIZE) {
            report.add_error("SlottedPage", "Payload size smaller than slotted header", pid);
            continue;
        }

        uint16_t sc = decode_u16_be(payload.data() + OFFSET_SLOT_COUNT);
        uint16_t fsp = decode_u16_be(payload.data() + OFFSET_FREE_SPACE_PTR);
        uint16_t dc = decode_u16_be(payload.data() + OFFSET_DELETED_SLOT_COUNT);

        if (dc > sc) {
            report.add_error("SlottedPage", "Deleted slot count (" + std::to_string(dc) +
                             ") exceeds total slot count (" + std::to_string(sc) + ")", pid);
            continue;
        }

        if (fsp > MAX_PAYLOAD_SIZE) {
            report.add_error("SlottedPage", "Free space pointer exceeds max payload: " +
                             std::to_string(fsp) + " > " + std::to_string(MAX_PAYLOAD_SIZE), pid);
            continue;
        }

        size_t slot_dir_end = SLOTTED_PAGE_HEADER_SIZE + static_cast<size_t>(sc) * SLOT_ENTRY_SIZE;
        if (slot_dir_end > fsp) {
            report.add_error("SlottedPage", "Slot directory overlaps free space pointer (" +
                             std::to_string(slot_dir_end) + " > " + std::to_string(fsp) + ")", pid);
        }
    }
}

void IntegrityChecker::check_catalogs_and_indexes(IntegrityReport& report) {
    BufferPoolManager bpm(page_file_, 64);

    auto tc_res = TableCatalog::open_or_create(bpm);
    if (tc_res.is_err()) {
        report.add_error("Catalog", "Failed to open TableCatalog: " + tc_res.status().message());
        return;
    }
    auto& tc = *tc_res.value();

    auto ic_res = IndexCatalog::open_or_create(bpm);
    if (ic_res.is_err()) {
        report.add_error("Catalog", "Failed to open IndexCatalog: " + ic_res.status().message());
        return;
    }
    auto& ic = *ic_res.value();

    auto tables = tc.list_tables();
    report.tables_checked = tables.size();

    auto indexes_res = ic.list_indexes();
    std::vector<IndexMetadata> indexes;
    if (indexes_res.is_ok()) {
        indexes = indexes_res.value();
    }
    report.indexes_checked = indexes.size();

    const uint32_t total_pages = page_file_.page_count();

    // Verify each table and audit all its records
    for (const auto& schema : tables) {
        std::set<std::string> seen_pks;
        std::vector<SqlType> expected_types;
        for (const auto& col : schema.columns) {
            expected_types.push_back(col.type);
        }

        for (page_id_t pid = 1; pid < total_pages; ++pid) {
            Page page;
            if (!page_file_.read_page(pid, page).is_ok()) continue;
            if (page.page_type() != PageType::Record || !SlottedPage::is_slotted_page(page)) continue;

            SlottedPage slotted(page);
            for (slot_id_t slot = 0; slot < slotted.slot_count(); ++slot) {
                auto span_res = slotted.get_record(slot);
                if (span_res.is_err()) continue;

                auto peek_res = SqlRow::peek_table_id(span_res.value());
                if (peek_res.is_err() || peek_res.value() != schema.table_id) continue;

                report.records_checked++;
                auto row_res = SqlRow::decode(span_res.value(), schema.table_id, expected_types);
                if (row_res.is_err()) {
                    report.add_error("Record", "Failed to decode row: " + row_res.status().message(), pid, schema.name);
                    continue;
                }

                const auto& row = row_res.value();

                // Check NOT NULL constraints
                for (size_t col_idx = 0; col_idx < schema.columns.size(); ++col_idx) {
                    const auto& col = schema.columns[col_idx];
                    if (schema.is_not_null(col.name) && row.get(col_idx).is_null()) {
                        report.add_error("Constraint", "NOT NULL violation on column '" + col.name + "'", pid, schema.name);
                    }
                }

                // Check PRIMARY KEY uniqueness
                const auto* pk_constraint = schema.find_primary_key();
                if (pk_constraint) {
                    auto pk_idx_opt = schema.find_column_index(pk_constraint->column_name);
                    if (pk_idx_opt.has_value()) {
                        std::string pk_str = row.get(*pk_idx_opt).to_string();
                        if (seen_pks.find(pk_str) != seen_pks.end()) {
                            report.add_error("Constraint", "Duplicate PRIMARY KEY value '" + pk_str + "'", pid, schema.name);
                        } else {
                            seen_pks.insert(pk_str);
                        }
                    }
                }
            }
        }
    }

    // Verify B+ Tree structural invariants
    for (const auto& idx_meta : indexes) {
        if (idx_meta.root_page_id == INVALID_PAGE_ID) {
            continue;
        }

        if (idx_meta.root_page_id >= total_pages) {
            report.add_error("IndexCatalog", "Index '" + idx_meta.name + "' has invalid root_page_id: " +
                             std::to_string(idx_meta.root_page_id), idx_meta.root_page_id, idx_meta.name);
            continue;
        }

        BPlusTree tree(bpm, idx_meta.root_page_id);
        auto it = tree.begin();
        std::optional<IndexKey> prev_key;
        std::set<std::string> unique_keys;

        while (it.is_valid()) {
            const auto& entry = *it;

            // Check non-decreasing order
            if (prev_key.has_value()) {
                if (entry.key < prev_key.value()) {
                    report.add_error("BPlusTree", "Keys out of order in index '" + idx_meta.name + "'",
                                     INVALID_PAGE_ID, idx_meta.name);
                }
            }
            prev_key = entry.key;

            // If UNIQUE index, verify uniqueness
            if (idx_meta.is_unique) {
                std::string k_str = entry.key.to_string();
                if (unique_keys.find(k_str) != unique_keys.end()) {
                    report.add_error("Constraint", "UNIQUE constraint violation in index '" +
                                     idx_meta.name + "' for key '" + k_str + "'",
                                     entry.rid.page_id, idx_meta.name);
                } else {
                    unique_keys.insert(k_str);
                }
            }

            if (entry.rid.page_id >= total_pages) {
                report.add_error("BPlusTree", "Index entry points to invalid page " +
                                 std::to_string(entry.rid.page_id), entry.rid.page_id, idx_meta.name);
            }

            ++it;
        }
    }
}

void IntegrityChecker::check_foreign_keys(IntegrityReport& report) {
    BufferPoolManager bpm(page_file_, 64);
    auto tc_res = TableCatalog::open_or_create(bpm);
    if (tc_res.is_err()) return;
    auto& tc = *tc_res.value();

    auto tables = tc.list_tables();
    const uint32_t total_pages = page_file_.page_count();

    // Map parent table primary keys
    std::map<std::string, std::set<std::string>> parent_pk_sets;
    for (const auto& schema : tables) {
        const auto* pk_constraint = schema.find_primary_key();
        if (!pk_constraint) continue;

        auto pk_idx_opt = schema.find_column_index(pk_constraint->column_name);
        if (!pk_idx_opt.has_value()) continue;
        size_t pk_idx = *pk_idx_opt;

        std::vector<SqlType> expected_types;
        for (const auto& col : schema.columns) expected_types.push_back(col.type);

        std::set<std::string> pks;
        for (page_id_t pid = 1; pid < total_pages; ++pid) {
            Page page;
            if (!page_file_.read_page(pid, page).is_ok()) continue;
            if (page.page_type() != PageType::Record || !SlottedPage::is_slotted_page(page)) continue;

            SlottedPage slotted(page);
            for (slot_id_t slot = 0; slot < slotted.slot_count(); ++slot) {
                auto span_res = slotted.get_record(slot);
                if (span_res.is_err()) continue;

                auto peek_res = SqlRow::peek_table_id(span_res.value());
                if (peek_res.is_err() || peek_res.value() != schema.table_id) continue;

                auto row_res = SqlRow::decode(span_res.value(), schema.table_id, expected_types);
                if (row_res.is_ok()) {
                    pks.insert(row_res.value().get(pk_idx).to_string());
                }
            }
        }
        parent_pk_sets[canonicalize_name(schema.name)] = std::move(pks);
    }

    // Verify foreign keys in each child table
    for (const auto& schema : tables) {
        auto fks = schema.find_foreign_keys();
        if (fks.empty()) continue;

        std::vector<SqlType> expected_types;
        for (const auto& col : schema.columns) expected_types.push_back(col.type);

        for (const auto* fk : fks) {
            std::string parent_canon = canonicalize_name(fk->ref_table_name);
            auto it = parent_pk_sets.find(parent_canon);
            if (it == parent_pk_sets.end()) {
                report.add_error("Constraint", "Foreign key references nonexistent parent table '" +
                                 fk->ref_table_name + "'", INVALID_PAGE_ID, schema.name);
                continue;
            }

            const auto& valid_pks = it->second;
            auto child_col_opt = schema.find_column_index(fk->column_name);
            if (!child_col_opt.has_value()) {
                report.add_error("Constraint", "Foreign key column '" + fk->column_name +
                                 "' not found in schema", INVALID_PAGE_ID, schema.name);
                continue;
            }
            size_t child_col_idx = *child_col_opt;

            for (page_id_t pid = 1; pid < total_pages; ++pid) {
                Page page;
                if (!page_file_.read_page(pid, page).is_ok()) continue;
                if (page.page_type() != PageType::Record || !SlottedPage::is_slotted_page(page)) continue;

                SlottedPage slotted(page);
                for (slot_id_t slot = 0; slot < slotted.slot_count(); ++slot) {
                    auto span_res = slotted.get_record(slot);
                    if (span_res.is_err()) continue;

                    auto peek_res = SqlRow::peek_table_id(span_res.value());
                    if (peek_res.is_err() || peek_res.value() != schema.table_id) continue;

                    auto row_res = SqlRow::decode(span_res.value(), schema.table_id, expected_types);
                    if (row_res.is_ok()) {
                        const auto& val = row_res.value().get(child_col_idx);
                        if (!val.is_null()) {
                            std::string val_str = val.to_string();
                            if (valid_pks.find(val_str) == valid_pks.end()) {
                                report.add_error("Constraint", "Foreign key violation: value '" + val_str +
                                                 "' does not exist in referenced table '" + fk->ref_table_name + "'",
                                                 pid, schema.name);
                            }
                        }
                    }
                }
            }
        }
    }
}

void IntegrityChecker::check_free_space_map(IntegrityReport& report) {
    Page p0;
    if (!page_file_.read_page(0, p0).is_ok()) return;
    auto meta_res = DatabaseMetadata::decode_from_page(p0);
    if (meta_res.is_err()) return;

    page_id_t fsm_pid = meta_res.value().fsm_page_id;
    if (fsm_pid == INVALID_PAGE_ID) {
        return; // FSM not yet initialized
    }

    if (fsm_pid >= page_file_.page_count()) {
        report.add_error("FSM", "FSM page ID (" + std::to_string(fsm_pid) +
                         ") is out of file bounds", fsm_pid);
        return;
    }

    report.fsm_pages_checked = 1;
    Page fsm_page;
    if (!page_file_.read_page(fsm_pid, fsm_page).is_ok()) {
        report.add_error("FSM", "Failed to read FSM page", fsm_pid);
        return;
    }

    if (fsm_page.page_type() != PageType::FreeSpaceMap) {
        report.add_error("FSM", "FSM page does not have PageType::FreeSpaceMap", fsm_pid);
    }
}

void IntegrityChecker::check_wal(IntegrityReport& report) {
    if (wal_ == nullptr) {
        std::filesystem::path wal_path = page_file_.path().string() + ".wal";
        std::error_code ec;
        if (!std::filesystem::exists(wal_path, ec)) {
            report.wal_checked = false;
            return;
        }
    }

    report.wal_checked = true;
    report.wal_healthy = true;

    std::filesystem::path wal_path = wal_ ? wal_->path() : std::filesystem::path(page_file_.path().string() + ".wal");
    std::error_code ec;
    auto sz = std::filesystem::file_size(wal_path, ec);
    if (ec) {
        report.wal_healthy = false;
        report.add_error("WAL", "Failed to read WAL file size: " + ec.message());
        return;
    }

    if (sz == 0) {
        report.wal_healthy = false;
        report.add_error("WAL", "WAL file is 0 bytes (corrupted or truncated without header)");
        return;
    }

    if (sz < WAL_HEADER_SIZE) {
        report.wal_healthy = false;
        report.add_error("WAL", "WAL file size is smaller than 32-byte WAL header (" + std::to_string(sz) + " bytes)");
        return;
    }

    // Validate WAL header and record sequence
    auto wal_open = WriteAheadLog::open_or_create(wal_path);
    if (wal_open.is_err()) {
        report.wal_healthy = false;
        report.add_error("WAL", "Failed to open WAL: " + wal_open.status().message());
        return;
    }

    auto recs_res = wal_open.value()->read_all_records();
    if (recs_res.is_err()) {
        report.wal_healthy = false;
        report.add_error("WAL", "WAL records corrupted: " + recs_res.status().message());
        return;
    }

    const auto& recs = recs_res.value();
    lsn_t prev_lsn = 0;
    for (const auto& r : recs) {
        if (r.lsn <= prev_lsn && prev_lsn != 0) {
            report.wal_healthy = false;
            report.add_error("WAL", "Monotonic LSN ordering violation: " + std::to_string(r.lsn) +
                             " <= " + std::to_string(prev_lsn));
        }
        prev_lsn = r.lsn;
    }
}

} // namespace klyrdb
