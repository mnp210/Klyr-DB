#include "klyrdb/backup/csv_migrator.hpp"
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
#include "klyrdb/sql/ast.hpp"
#include "klyrdb/concurrency/process_file_lock.hpp"
#include "klyrdb/concurrency/lock_manager.hpp"
#include <fstream>
#include <sstream>
#include <charconv>
#include <algorithm>

#ifdef _WIN32
#include <process.h>
#define GETPID _getpid
#else
#include <unistd.h>
#define GETPID getpid
#endif

namespace klyrdb {

namespace {

std::string format_csv_field(const SqlValue& val, const std::string& null_str) {
    if (val.is_null()) {
        return null_str;
    }
    if (val.is_integer()) {
        return std::to_string(val.as_integer());
    }

    const std::string& s = val.as_text();
    if (s.empty()) {
        return "\"\"";
    }

    bool needs_quotes = (s == null_str ||
                         s.find(',') != std::string::npos ||
                         s.find('"') != std::string::npos ||
                         s.find('\n') != std::string::npos ||
                         s.find('\r') != std::string::npos);

    if (!needs_quotes) {
        return s;
    }

    std::string out = "\"";
    for (char c : s) {
        if (c == '"') {
            out += "\"\"";
        } else {
            out += c;
        }
    }
    out += "\"";
    return out;
}

struct ParsedCsvField {
    std::string value;
    bool was_quoted{false};
};

class StreamingCsvParser {
public:
    explicit StreamingCsvParser(std::istream& in) : in_(in) {}

    std::optional<std::vector<ParsedCsvField>> next_record() {
        if (in_.peek() == std::char_traits<char>::eof()) {
            return std::nullopt;
        }

        std::vector<ParsedCsvField> record;
        std::string current_field;
        bool in_quotes = false;
        bool was_quoted = false;
        bool field_has_content = false;
        char c;

        while (in_.get(c)) {
            if (in_quotes) {
                if (c == '"') {
                    if (in_.peek() == '"') {
                        // Escaped quote
                        char next_c;
                        in_.get(next_c);
                        current_field.push_back('"');
                        field_has_content = true;
                    } else {
                        // Closing quote
                        in_quotes = false;
                    }
                } else {
                    current_field.push_back(c);
                    field_has_content = true;
                }
                continue;
            }

            // Outside quotes
            if (c == '"' && !field_has_content && current_field.empty()) {
                in_quotes = true;
                was_quoted = true;
                continue;
            }

            if (c == ',') {
                record.push_back(ParsedCsvField{std::move(current_field), was_quoted});
                current_field.clear();
                was_quoted = false;
                field_has_content = false;
                continue;
            }

            if (c == '\r') {
                if (in_.peek() == '\n') {
                    in_.get(); // consume '\n'
                }
                record.push_back(ParsedCsvField{std::move(current_field), was_quoted});
                return record;
            }

            if (c == '\n') {
                record.push_back(ParsedCsvField{std::move(current_field), was_quoted});
                return record;
            }

            current_field.push_back(c);
            field_has_content = true;
        }

        if (field_has_content || was_quoted || !record.empty()) {
            record.push_back(ParsedCsvField{std::move(current_field), was_quoted});
            return record;
        }

        return std::nullopt;
    }

private:
    std::istream& in_;
};

} // namespace

Result<CsvResult> CsvMigrator::export_csv(
    const std::filesystem::path& db_path,
    const std::string& table_name,
    const std::filesystem::path& output_path,
    const CsvExportOptions& options) const {

    auto start_time = std::chrono::steady_clock::now();

    std::error_code ec;
    if (!std::filesystem::exists(db_path, ec)) {
        return Status::file_not_found("Database file not found: " + db_path.string());
    }

    // Acquire shared lock
    ProcessFileLock proc_lock(db_path.string() + ".lock");
    Status lock_st = proc_lock.lock_with_timeout(ProcessLockMode::Shared, options.lock_timeout);
    if (!lock_st.is_ok()) {
        return Status::database_busy("Failed to acquire shared lock on database: " + lock_st.message());
    }

    auto pf_res = PageFile::open(db_path, false);
    if (pf_res.is_err()) return pf_res.status();

    auto& pf = *pf_res.value();
    BufferPoolManager bpm(pf, 64);
    auto fsm_res = FreeSpaceMap::open_or_create(bpm);
    auto tc_res = TableCatalog::open_or_create(bpm);
    auto ic_res = IndexCatalog::open_or_create(bpm);

    if (tc_res.is_err()) return tc_res.status();
    if (ic_res.is_err()) return ic_res.status();

    auto& tc = *tc_res.value();
    auto tbl_res = tc.get_table(table_name);
    if (tbl_res.is_err()) {
        return tbl_res.status();
    }
    const auto& tbl = tbl_res.value();

    RecordManager rm(bpm, fsm_res.is_ok() ? fsm_res.value().get() : nullptr);
    SqlExecutor executor(bpm, rm, tc, *ic_res.value());

    // Create temporary output file
    const auto parent_dir = output_path.has_parent_path() ? output_path.parent_path() : std::filesystem::current_path();
    std::filesystem::create_directories(parent_dir, ec);

    const auto tmp_out_path = parent_dir / (output_path.filename().string() + ".tmp." +
                                            std::to_string(GETPID()) + "." + std::to_string(current_epoch_seconds()));

    std::ofstream ofs(tmp_out_path, std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) {
        return Status::io_error("Failed to open temporary CSV output file: " + tmp_out_path.string());
    }

    bool committed = false;
    struct StagingGuard {
        std::filesystem::path p;
        bool& is_committed;
        ~StagingGuard() {
            if (!is_committed) {
                std::error_code e;
                std::filesystem::remove(p, e);
            }
        }
    } staging_guard{tmp_out_path, committed};

    // 1. Header
    if (options.include_header) {
        for (size_t i = 0; i < tbl.columns.size(); ++i) {
            ofs << tbl.columns[i].name << (i + 1 < tbl.columns.size() ? "," : "\n");
        }
    }

    // 2. Rows
    auto q_res = executor.execute("SELECT * FROM " + tbl.name + ";");
    if (q_res.is_err()) {
        return q_res.status();
    }

    const auto& q = q_res.value();
    size_t rows_exported = 0;
    for (const auto& r : q.rows()) {
        for (size_t i = 0; i < tbl.columns.size(); ++i) {
            ofs << format_csv_field(r.get(i), options.null_string) << (i + 1 < tbl.columns.size() ? "," : "\n");
        }
        rows_exported++;
    }

    ofs.flush();
    if (ofs.fail()) {
        return Status::io_error("Failed to flush CSV file to disk: " + tmp_out_path.string());
    }
    ofs.close();

    std::filesystem::rename(tmp_out_path, output_path, ec);
    if (ec) {
        return Status::io_error("Failed to rename temporary CSV file to target: " + ec.message());
    }

    committed = true;

    auto end_time = std::chrono::steady_clock::now();
    CsvResult res;
    res.rows_processed = rows_exported;
    res.duration_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    return res;
}

Result<CsvResult> CsvMigrator::import_csv(
    const std::filesystem::path& input_path,
    const std::filesystem::path& db_path,
    const std::string& table_name,
    const CsvImportOptions& options) const {

    auto start_time = std::chrono::steady_clock::now();

    std::error_code ec;
    if (!std::filesystem::exists(input_path, ec)) {
        return Status::file_not_found("CSV input file not found: " + input_path.string());
    }
    if (!std::filesystem::exists(db_path, ec)) {
        return Status::file_not_found("Database file not found: " + db_path.string());
    }

    std::ifstream ifs(input_path, std::ios::in | std::ios::binary);
    if (!ifs.is_open()) {
        return Status::io_error("Failed to open CSV input file: " + input_path.string());
    }

    // Acquire exclusive lock on database
    ProcessFileLock proc_lock(db_path.string() + ".lock");
    Status lock_st = proc_lock.lock_with_timeout(ProcessLockMode::Exclusive, options.lock_timeout);
    if (!lock_st.is_ok()) {
        return Status::database_busy("Failed to acquire exclusive lock on database: " + lock_st.message());
    }

    auto pf_res = PageFile::open(db_path, false);
    if (pf_res.is_err()) return pf_res.status();

    auto& pf = *pf_res.value();
    BufferPoolManager bpm(pf, 64);
    auto wal_path = db_path.string() + ".wal";
    auto wal_res = WriteAheadLog::open_or_create(wal_path);
    if (wal_res.is_err()) return wal_res.status();

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

    auto tbl_res = tc.get_table(table_name);
    if (tbl_res.is_err()) {
        return tbl_res.status();
    }
    const auto& tbl = tbl_res.value();

    RecordManager rm(bpm, fsm_res.is_ok() ? fsm_res.value().get() : nullptr);
    SqlExecutor executor(bpm, rm, tc, ic);
    executor.set_transaction_manager(&txn_mgr);

    // Begin transaction for CSV import
    auto txn_res = txn_mgr.begin();
    if (txn_res.is_err()) return txn_res.status();
    auto txn = txn_res.value();
    txn_mgr.set_current_transaction(txn);

    StreamingCsvParser parser(ifs);
    size_t line_number = 0;
    size_t rows_imported = 0;

    // Optional header processing
    if (options.has_header) {
        auto hdr_opt = parser.next_record();
        line_number++;
        if (!hdr_opt.has_value()) {
            (void)txn_mgr.rollback(txn);
            return Status::invalid_argument("CSV file is empty or missing header");
        }
        const auto& hdr = *hdr_opt;
        if (hdr.size() != tbl.columns.size()) {
            (void)txn_mgr.rollback(txn);
            return Status::invalid_argument("CSV header column count (" + std::to_string(hdr.size()) +
                                            ") does not match table column count (" +
                                            std::to_string(tbl.columns.size()) + ")");
        }
    }

    // Process data rows
    while (auto rec_opt = parser.next_record()) {
        line_number++;
        const auto& rec = *rec_opt;
        if (rec.empty()) continue; // skip blank line

        if (rec.size() != tbl.columns.size()) {
            (void)txn_mgr.rollback(txn);
            return Status::invalid_argument("CSV row #" + std::to_string(line_number) +
                                            " column count mismatch: expected " +
                                            std::to_string(tbl.columns.size()) +
                                            ", got " + std::to_string(rec.size()));
        }

        std::vector<SqlValue> row_vals;
        row_vals.reserve(rec.size());

        for (size_t i = 0; i < rec.size(); ++i) {
            const auto& fld = rec[i];
            const auto& col = tbl.columns[i];

            if (!fld.was_quoted && fld.value == options.null_string) {
                row_vals.push_back(SqlValue::null());
            } else if (col.type == SqlType::Integer) {
                if (fld.value.empty() && !fld.was_quoted) {
                    row_vals.push_back(SqlValue::null());
                } else {
                    int64_t ival = 0;
                    auto [p, errc] = std::from_chars(fld.value.data(), fld.value.data() + fld.value.size(), ival);
                    if (errc != std::errc() || p != fld.value.data() + fld.value.size()) {
                        (void)txn_mgr.rollback(txn);
                        return Status::type_mismatch("Invalid integer value '" + fld.value +
                                                     "' at CSV line #" + std::to_string(line_number) +
                                                     " for column '" + col.name + "'");
                    }
                    row_vals.emplace_back(ival);
                }
            } else {
                // Text
                if (!fld.was_quoted && fld.value == options.null_string) {
                    row_vals.push_back(SqlValue::null());
                } else {
                    row_vals.emplace_back(fld.value);
                }
            }
        }

        // Execute insert via SqlExecutor AST
        InsertStatement ins;
        ins.table_name = tbl.name;
        for (const auto& col : tbl.columns) {
            ins.column_names.push_back(col.name);
        }
        ins.values = std::move(row_vals);

        auto ins_res = executor.execute(ins);
        if (ins_res.is_err()) {
            (void)txn_mgr.rollback(txn);
            return Status::constraint_violation("Constraint violation on CSV line #" +
                                                std::to_string(line_number) + ": " +
                                                ins_res.status().message());
        }
        rows_imported++;
    }

    // Commit transaction
    auto commit_st = txn_mgr.commit(txn);
    if (!commit_st.is_ok()) {
        return commit_st;
    }

    (void)bpm.flush_all_pages();
    (void)pf.sync();
    (void)wal.flush();

    auto end_time = std::chrono::steady_clock::now();
    CsvResult res;
    res.rows_processed = rows_imported;
    res.duration_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    return res;
}

} // namespace klyrdb
