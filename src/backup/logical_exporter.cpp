#include "klyrdb/backup/logical_exporter.hpp"
#include "klyrdb/backup/backup_manifest.hpp"
#include "klyrdb/storage/page_file.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/storage/free_space_map.hpp"
#include "klyrdb/sql/executor.hpp"
#include "klyrdb/concurrency/process_file_lock.hpp"
#include "klyrdb/concurrency/lock_manager.hpp"
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <queue>
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

std::string escape_sql_string(std::string_view text) {
    std::string out = "'";
    for (char c : text) {
        if (c == '\'') {
            out += "''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

std::string format_sql_value(const SqlValue& val) {
    if (val.is_null()) {
        return "NULL";
    }
    if (val.is_integer()) {
        return std::to_string(val.as_integer());
    }
    return escape_sql_string(val.as_text());
}

std::vector<TableSchema> sort_tables_topologically(const std::vector<TableSchema>& tables) {
    std::unordered_map<std::string, const TableSchema*> name_to_schema;
    std::unordered_map<std::string, std::unordered_set<std::string>> graph; // parent -> children
    std::unordered_map<std::string, size_t> in_degree;

    for (const auto& tbl : tables) {
        std::string lower = canonicalize_name(tbl.name);
        name_to_schema[lower] = &tbl;
        in_degree[lower] = 0;
        graph[lower] = {};
    }

    for (const auto& tbl : tables) {
        std::string child = canonicalize_name(tbl.name);
        for (const auto& c : tbl.constraints) {
            if (c.type == ConstraintType::ForeignKey) {
                std::string parent = canonicalize_name(c.ref_table_name);
                if (parent != child && name_to_schema.find(parent) != name_to_schema.end()) {
                    if (graph[parent].insert(child).second) {
                        in_degree[child]++;
                    }
                }
            }
        }
    }

    std::queue<std::string> q;
    for (const auto& [name, deg] : in_degree) {
        if (deg == 0) {
            q.push(name);
        }
    }

    std::vector<TableSchema> sorted;
    sorted.reserve(tables.size());
    std::unordered_set<std::string> visited;

    while (!q.empty()) {
        std::string curr = q.front();
        q.pop();

        if (visited.insert(curr).second) {
            sorted.push_back(*name_to_schema[curr]);
            for (const auto& child : graph[curr]) {
                if (--in_degree[child] == 0) {
                    q.push(child);
                }
            }
        }
    }

    // Append any circular dependency leftovers
    for (const auto& tbl : tables) {
        std::string lower = canonicalize_name(tbl.name);
        if (visited.find(lower) == visited.end()) {
            sorted.push_back(tbl);
        }
    }

    return sorted;
}

} // namespace

Result<ExportResult> LogicalExporter::export_sql(
    const std::filesystem::path& db_path,
    const std::filesystem::path& output_path,
    const ExportOptions& options) const {

    auto start_time = std::chrono::steady_clock::now();

    // 1. Validate database file exists
    std::error_code ec;
    if (!std::filesystem::exists(db_path, ec)) {
        return Status::file_not_found("Database file not found: " + db_path.string());
    }

    // 2. Acquire shared read lock on database
    ProcessFileLock proc_lock(db_path.string() + ".lock");
    Status lock_st = proc_lock.lock_with_timeout(ProcessLockMode::Shared, options.lock_timeout);
    if (!lock_st.is_ok()) {
        return Status::database_busy("Failed to acquire shared lock on source database: " + lock_st.message());
    }

    auto in_proc_lock = LockManager::instance().get_lock(db_path, false);
    std::optional<SharedLockGuard> shared_guard;
    if (in_proc_lock) {
        auto g_res = SharedLockGuard::acquire(*in_proc_lock, options.lock_timeout);
        if (g_res.is_err()) {
            return Status::database_busy("Failed to acquire in-process shared database lock: " + g_res.status().message());
        }
        shared_guard = std::move(g_res.value());
    }

    // 3. Open database
    auto pf_res = PageFile::open(db_path, false);
    if (pf_res.is_err()) {
        return pf_res.status();
    }
    auto& pf = *pf_res.value();
    BufferPoolManager bpm(pf, 64);
    auto fsm_res = FreeSpaceMap::open_or_create(bpm);
    auto tc_res = TableCatalog::open_or_create(bpm);
    auto ic_res = IndexCatalog::open_or_create(bpm);

    if (tc_res.is_err()) return tc_res.status();
    if (ic_res.is_err()) return ic_res.status();

    auto& tc = *tc_res.value();
    auto& ic = *ic_res.value();
    RecordManager rm(bpm, fsm_res.is_ok() ? fsm_res.value().get() : nullptr);
    SqlExecutor executor(bpm, rm, tc, ic);

    // 4. Create temporary output file
    const auto parent_dir = output_path.has_parent_path() ? output_path.parent_path() : std::filesystem::current_path();
    std::filesystem::create_directories(parent_dir, ec);

    const auto tmp_out_path = parent_dir / (output_path.filename().string() + ".tmp." +
                                            std::to_string(GETPID()) + "." + std::to_string(current_epoch_seconds()));

    std::ofstream ofs(tmp_out_path, std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) {
        return Status::io_error("Failed to open output SQL dump file for writing: " + tmp_out_path.string());
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

    // 5. Header comments
    ofs << "-- ====================================================================\n";
    ofs << "-- KLYRDB LOGICAL SQL DUMP\n";
    ofs << "-- Source Database: " << db_path.filename().string() << "\n";
    ofs << "-- Export Timestamp (UTC): " << current_utc_timestamp_iso8601() << "\n";
    ofs << "-- Format Version: 1\n";
    ofs << "-- ====================================================================\n\n";

    auto raw_tables = tc.list_tables();
    auto tables = sort_tables_topologically(raw_tables);

    size_t tables_exported = 0;
    size_t rows_exported = 0;
    size_t indexes_exported = 0;

    // 6. Schema DDL Export
    if (options.include_schema) {
        ofs << "-- --------------------------------------------------------------------\n";
        ofs << "-- TABLE SCHEMAS\n";
        ofs << "-- --------------------------------------------------------------------\n";

        for (const auto& tbl : tables) {
            ofs << "CREATE TABLE " << tbl.name << " (\n";
            std::vector<std::string> lines;

            // Column definitions
            for (const auto& col : tbl.columns) {
                std::string col_line = "    " + col.name + " " + to_string(col.type);

                bool is_pk = false;
                for (const auto& c : tbl.constraints) {
                    if (c.column_name == col.name && c.type == ConstraintType::PrimaryKey) {
                        is_pk = true;
                        break;
                    }
                }

                if (is_pk) {
                    col_line += " PRIMARY KEY";
                } else {
                    for (const auto& c : tbl.constraints) {
                        if (c.column_name == col.name) {
                            if (c.type == ConstraintType::NotNull) {
                                col_line += " NOT NULL";
                            } else if (c.type == ConstraintType::Unique) {
                                col_line += " UNIQUE";
                            }
                        }
                    }
                }

                lines.push_back(std::move(col_line));
            }

            // Table constraints (e.g. FOREIGN KEY)
            for (const auto& c : tbl.constraints) {
                if (c.type == ConstraintType::ForeignKey) {
                    std::string fk_line = "    FOREIGN KEY (" + c.column_name + ") REFERENCES " +
                                          c.ref_table_name + "(" + c.ref_column_name + ")";
                    lines.push_back(std::move(fk_line));
                }
            }

            for (size_t i = 0; i < lines.size(); ++i) {
                ofs << lines[i] << (i + 1 < lines.size() ? ",\n" : "\n");
            }
            ofs << ");\n\n";
            tables_exported++;
        }
    }

    // 7. Table Data Export
    if (options.include_data) {
        ofs << "-- --------------------------------------------------------------------\n";
        ofs << "-- TABLE DATA\n";
        ofs << "-- --------------------------------------------------------------------\n";

        if (options.transactional) {
            ofs << "BEGIN TRANSACTION;\n\n";
        }

        for (const auto& tbl : tables) {
            auto q_res = executor.execute("SELECT * FROM " + tbl.name + ";");
            if (q_res.is_err()) {
                return q_res.status();
            }

            const auto& q = q_res.value();
            if (q.row_count() > 0) {
                ofs << "-- Data for table: " << tbl.name << " (" << q.row_count() << " rows)\n";
                for (const auto& r : q.rows()) {
                    ofs << "INSERT INTO " << tbl.name << " (";
                    for (size_t i = 0; i < tbl.columns.size(); ++i) {
                        ofs << tbl.columns[i].name << (i + 1 < tbl.columns.size() ? ", " : "");
                    }
                    ofs << ") VALUES (";
                    for (size_t i = 0; i < tbl.columns.size(); ++i) {
                        ofs << format_sql_value(r.get(i)) << (i + 1 < tbl.columns.size() ? ", " : "");
                    }
                    ofs << ");\n";
                    rows_exported++;
                }
                ofs << "\n";
            }
        }

        if (options.transactional) {
            ofs << "COMMIT;\n\n";
        }
    }

    // 8. Secondary Indexes Export (Post-Data)
    if (options.include_indexes) {
        bool wrote_idx_header = false;
        for (const auto& tbl : tables) {
            for (const auto& idx : tbl.indexes) {
                // Check if this index was auto-generated to enforce a constraint
                bool is_constraint_index = false;
                for (const auto& c : tbl.constraints) {
                    if (c.enforcement_index_name == idx.index_name) {
                        is_constraint_index = true;
                        break;
                    }
                }

                if (!is_constraint_index) {
                    if (!wrote_idx_header) {
                        ofs << "-- --------------------------------------------------------------------\n";
                        ofs << "-- STANDALONE SECONDARY INDEXES\n";
                        ofs << "-- --------------------------------------------------------------------\n";
                        wrote_idx_header = true;
                    }
                    ofs << "CREATE INDEX " << idx.index_name << " ON " << tbl.name
                        << "(" << idx.column_name << ");\n";
                    indexes_exported++;
                }
            }
        }
        if (wrote_idx_header) {
            ofs << "\n";
        }
    }

    ofs.flush();
    if (ofs.fail()) {
        return Status::io_error("Failed to write SQL dump to disk: " + tmp_out_path.string());
    }
    ofs.close();

    // 10. Atomic publication
    std::filesystem::rename(tmp_out_path, output_path, ec);
    if (ec) {
        return Status::io_error("Failed to rename temporary SQL dump to target: " + ec.message());
    }

    committed = true;

    auto end_time = std::chrono::steady_clock::now();
    ExportResult res;
    res.output_path = output_path;
    res.tables_exported = tables_exported;
    res.rows_exported = rows_exported;
    res.indexes_exported = indexes_exported;
    res.duration_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    return res;
}

} // namespace klyrdb
