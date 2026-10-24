#include "klyrdb/sql/executor.hpp"
#include "klyrdb/sql/parser.hpp"
#include "klyrdb/txn/transaction_manager.hpp"
#include "klyrdb/diagnostics/metrics.hpp"
#include "klyrdb/catalog/table_statistics.hpp"
#include <algorithm>
#include <unordered_set>
#include <unordered_map>

namespace klyrdb {

SqlExecutor::~SqlExecutor() {
    if (owns_explicit_txn_lock_ && db_lock_) {
        (void)db_lock_->release_exclusive();
        owns_explicit_txn_lock_ = false;
    }
}

Result<QueryResult> SqlExecutor::execute(std::string_view sql) {
    auto parse_res = SqlParser::parse_sql(sql);
    if (parse_res.is_err()) {
        return parse_res.status();
    }
    return execute(*parse_res.value());
}

Result<QueryResult> SqlExecutor::execute(const Statement& stmt) {
    if (stmt.type() == StatementType::Begin) {
        if (txn_mgr_ == nullptr) {
            return Status::invalid_argument("No transaction manager configured");
        }
        if (txn_mgr_->current_transaction() != nullptr) {
            return Status::invalid_transaction_state("Transaction already in progress");
        }
        if (db_lock_) {
            Status l_st = db_lock_->acquire_exclusive(lock_timeout_);
            if (!l_st.is_ok()) return l_st;
        }
        auto txn_res = txn_mgr_->begin();
        if (txn_res.is_err()) {
            if (db_lock_) (void)db_lock_->release_exclusive();
            return txn_res.status();
        }
        owns_explicit_txn_lock_ = true;
        txn_mgr_->set_current_transaction(txn_res.value());
        return QueryResult::make_message("BEGIN transaction " + std::to_string(txn_res.value()->txn_id()));
    }

    if (stmt.type() == StatementType::Commit) {
        if (txn_mgr_ == nullptr) {
            return Status::invalid_argument("No transaction manager configured");
        }
        if (txn_mgr_->current_transaction() == nullptr) {
            return Status::invalid_transaction_state("No active transaction to commit");
        }
        auto cur = txn_mgr_->current_transaction();
        Status st = txn_mgr_->commit(cur);
        if (owns_explicit_txn_lock_ && db_lock_) {
            owns_explicit_txn_lock_ = false;
            (void)db_lock_->release_exclusive();
        }
        if (st.is_err()) return st;
        return QueryResult::make_message("COMMIT completed");
    }

    if (stmt.type() == StatementType::Rollback) {
        if (txn_mgr_ == nullptr) {
            return Status::invalid_argument("No transaction manager configured");
        }
        if (txn_mgr_->current_transaction() == nullptr) {
            return Status::invalid_transaction_state("No active transaction to rollback");
        }
        auto cur = txn_mgr_->current_transaction();
        Status st = txn_mgr_->rollback(cur);
        if (owns_explicit_txn_lock_ && db_lock_) {
            owns_explicit_txn_lock_ = false;
            (void)db_lock_->release_exclusive();
        }
        if (st.is_err()) return st;
        return QueryResult::make_message("ROLLBACK completed");
    }

    // Acquire shared or exclusive lock based on statement type
    std::optional<SharedLockGuard> shared_guard;
    std::optional<ExclusiveLockGuard> excl_guard;

    const bool is_read_only = (stmt.type() == StatementType::Select ||
                               stmt.type() == StatementType::Explain ||
                               stmt.type() == StatementType::Analyze);

    if (db_lock_) {
        if (is_read_only) {
            auto g_res = SharedLockGuard::acquire(*db_lock_, lock_timeout_);
            if (g_res.is_err()) return g_res.status();
            shared_guard = std::move(g_res.value());
        } else {
            auto g_res = ExclusiveLockGuard::acquire(*db_lock_, lock_timeout_);
            if (g_res.is_err()) return g_res.status();
            excl_guard = std::move(g_res.value());
        }
    }

    // Reject DDL inside explicit transaction
    if (txn_mgr_ != nullptr && txn_mgr_->current_transaction() != nullptr) {
        if (stmt.type() == StatementType::CreateTable ||
            stmt.type() == StatementType::CreateIndex ||
            stmt.type() == StatementType::DropTable ||
            stmt.type() == StatementType::DropIndex) {
            return Status::invalid_transaction_state("DDL operations (CREATE/DROP) are not supported inside an explicit transaction block");
        }
    }

    // Check if statement needs an implicit transaction
    const bool is_mutation = (stmt.type() == StatementType::Insert ||
                              stmt.type() == StatementType::Update ||
                              stmt.type() == StatementType::Delete);

    const bool run_in_implicit_txn = (txn_mgr_ != nullptr &&
                                      txn_mgr_->current_transaction() == nullptr &&
                                      is_mutation);

    std::shared_ptr<Transaction> implicit_txn = nullptr;
    if (run_in_implicit_txn) {
        auto begin_res = txn_mgr_->begin();
        if (begin_res.is_err()) return begin_res.status();
        implicit_txn = begin_res.value();
        txn_mgr_->set_current_transaction(implicit_txn);
    }

    auto start_time = std::chrono::steady_clock::now();
    auto res = execute_internal(stmt);
    auto end_time = std::chrono::steady_clock::now();
    uint64_t dur_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();

    if (bpm_.metrics() != nullptr) {
        bpm_.metrics()->record_statement_executed(res.is_ok(), dur_us);
    }

    if (run_in_implicit_txn) {
        if (res.is_err()) {
            (void)txn_mgr_->rollback(implicit_txn);
        } else {
            Status c_st = txn_mgr_->commit(implicit_txn);
            if (c_st.is_err()) {
                return c_st;
            }
        }
    }

    return res;
}

Result<QueryResult> SqlExecutor::execute_internal(const Statement& stmt) {
    switch (stmt.type()) {
        case StatementType::CreateTable:
            return execute_create_table(static_cast<const CreateTableStatement&>(stmt));
        case StatementType::CreateIndex:
            return execute_create_index(static_cast<const CreateIndexStatement&>(stmt));
        case StatementType::DropTable:
            return execute_drop_table(static_cast<const DropTableStatement&>(stmt));
        case StatementType::DropIndex:
            return execute_drop_index(static_cast<const DropIndexStatement&>(stmt));
        case StatementType::Insert:
            return execute_insert(static_cast<const InsertStatement&>(stmt));
        case StatementType::Select:
            return execute_select(static_cast<const SelectStatement&>(stmt));
        case StatementType::Update:
            return execute_update(static_cast<const UpdateStatement&>(stmt));
        case StatementType::Delete:
            return execute_delete(static_cast<const DeleteStatement&>(stmt));
        case StatementType::Explain:
            return execute_explain(static_cast<const ExplainStatement&>(stmt));
        case StatementType::Analyze:
            return execute_analyze(static_cast<const AnalyzeStatement&>(stmt));
        default:
            return Status::unsupported_sql("Unknown statement type");
    }
}

Result<QueryResult> SqlExecutor::execute_create_table(const CreateTableStatement& stmt) {
    auto create_res = tc_.create_table(stmt.table_name, stmt.columns, stmt.constraints);
    if (create_res.is_err()) {
        return create_res.status();
    }
    const auto& tbl = create_res.value();

    // Create backing unique indexes for PRIMARY KEY and UNIQUE constraints
    for (const auto& c : tbl.constraints) {
        if (c.type == ConstraintType::PrimaryKey || c.type == ConstraintType::Unique) {
            std::string canonical_idx = canonicalize_name(c.name);
            auto col_opt = tbl.find_column_index(c.column_name);
            if (!col_opt.has_value()) continue;
            size_t col_idx = *col_opt;
            const auto& col = tbl.columns[col_idx];

            KeyType key_type = (col.type == SqlType::Integer) ? KeyType::Int64 : KeyType::String;
            Status cat_st = ic_.create_index(canonical_idx, key_type, INVALID_PAGE_ID, true /*is_unique*/);
            if (!cat_st.is_ok()) {
                (void)tc_.drop_table(tbl.name);
                return cat_st;
            }

            TableIndexInfo idx_info;
            idx_info.index_name = canonical_idx;
            idx_info.column_name = col.name;
            idx_info.column_index = col_idx;
            idx_info.root_page_id = INVALID_PAGE_ID;
            idx_info.is_unique = true;

            Status tc_st = tc_.add_index_to_table(tbl.name, idx_info);
            if (!tc_st.is_ok()) {
                (void)ic_.drop_index(canonical_idx);
                (void)tc_.drop_table(tbl.name);
                return tc_st;
            }
        }
    }

    return QueryResult::make_message("Table '" + tbl.name + "' created.");
}

Result<QueryResult> SqlExecutor::execute_create_index(const CreateIndexStatement& stmt) {
    auto tbl_res = tc_.get_table(stmt.table_name);
    if (tbl_res.is_err()) return tbl_res.status();
    const auto& tbl = tbl_res.value();

    auto col_idx_opt = tbl.find_column_index(stmt.column_name);
    if (!col_idx_opt.has_value()) {
        return Status::column_not_found("Column '" + stmt.column_name + "' not found in table '" +
                                        tbl.name + "'");
    }
    size_t col_idx = *col_idx_opt;
    const auto& col = tbl.columns[col_idx];

    KeyType key_type = (col.type == SqlType::Integer) ? KeyType::Int64 : KeyType::String;

    std::string canonical_idx = canonicalize_name(stmt.index_name);
    if (ic_.get_index(canonical_idx).is_ok()) {
        return Status::duplicate_key("Index '" + canonical_idx + "' already exists");
    }

    BPlusTree tree(bpm_, INVALID_PAGE_ID);

    Status cat_st = ic_.create_index(canonical_idx, key_type, INVALID_PAGE_ID, stmt.is_unique);
    if (!cat_st.is_ok()) return cat_st;

    tree.set_on_root_change([this, canonical_idx](page_id_t new_root) {
        (void)ic_.update_root_page(canonical_idx, new_root);
    });

    // Populate index with existing rows
    auto rows_res = find_matching_rows(tbl, WhereClause{});
    if (rows_res.is_err()) {
        (void)ic_.drop_index(canonical_idx);
        return rows_res.status();
    }

    for (const auto& item : rows_res.value()) {
        const auto& val = item.row.get(col_idx);
        if (val.is_null()) {
            continue; // Do not index NULL values
        }
        IndexKey key = val.to_index_key();
        Status ins_st = tree.insert(key, item.rid);
        if (!ins_st.is_ok()) {
            (void)ic_.drop_index(canonical_idx);
            return ins_st;
        }
    }

    // Attach index to TableCatalog
    TableIndexInfo idx_info;
    idx_info.index_name = canonical_idx;
    idx_info.column_name = col.name;
    idx_info.column_index = col_idx;
    idx_info.root_page_id = tree.root_page_id();
    idx_info.is_unique = stmt.is_unique;

    Status tc_st = tc_.add_index_to_table(tbl.name, idx_info);
    if (!tc_st.is_ok()) {
        (void)ic_.drop_index(canonical_idx);
        return tc_st;
    }

    if (tree.root_page_id() != INVALID_PAGE_ID) {
        (void)ic_.update_root_page(canonical_idx, tree.root_page_id());
    }

    return QueryResult::make_message("Index '" + canonical_idx + "' created on " + tbl.name +
                                     " (" + col.name + ").");
}

Result<QueryResult> SqlExecutor::execute_drop_table(const DropTableStatement& stmt) {
    auto tbl_res = tc_.get_table(stmt.table_name);
    if (tbl_res.is_err()) return tbl_res.status();
    const auto& tbl = tbl_res.value();

    // Check if any other table has a FOREIGN KEY referencing this table
    for (const auto& other_tbl : tc_.list_tables()) {
        if (canonicalize_name(other_tbl.name) == canonicalize_name(tbl.name)) {
            continue; // Self-referencing table can be dropped
        }
        for (const auto* fk : other_tbl.find_foreign_keys()) {
            if (canonicalize_name(fk->ref_table_name) == canonicalize_name(tbl.name)) {
                return Status::constraint_violation(
                    "Cannot drop table '" + tbl.name + "' because it is referenced by foreign key '" +
                    fk->name + "' in table '" + other_tbl.name + "'");
            }
        }
    }

    // Drop attached indexes from IndexCatalog
    for (const auto& idx : tbl.indexes) {
        (void)ic_.drop_index(idx.index_name);
    }

    // Delete all records of this table
    auto rows_res = find_matching_rows(tbl, WhereClause{});
    if (rows_res.is_ok()) {
        for (const auto& item : rows_res.value()) {
            (void)rm_.delete_record(item.rid);
        }
    }

    Status drop_st = tc_.drop_table(tbl.name);
    if (!drop_st.is_ok()) return drop_st;

    return QueryResult::make_message("Table '" + tbl.name + "' dropped.");
}

Result<QueryResult> SqlExecutor::execute_drop_index(const DropIndexStatement& stmt) {
    std::string canonical_idx = canonicalize_name(stmt.index_name);

    std::string tbl_name = stmt.table_name;
    if (tbl_name.empty()) {
        // Search which table owns this index
        for (const auto& t : tc_.list_tables()) {
            if (t.find_index_by_name(canonical_idx) != nullptr) {
                tbl_name = t.name;
                break;
            }
            for (const auto& c : t.constraints) {
                if (canonicalize_name(c.name) == canonical_idx ||
                    canonicalize_name(c.enforcement_index_name) == canonical_idx) {
                    tbl_name = t.name;
                    break;
                }
            }
            if (!tbl_name.empty()) break;
        }
    }

    if (tbl_name.empty()) {
        return Status::index_not_found("Index '" + canonical_idx + "' not found");
    }

    auto tbl_res = tc_.get_table(tbl_name);
    if (tbl_res.is_err()) return tbl_res.status();
    const auto& tbl = tbl_res.value();

    // Protect indexes that enforce PRIMARY KEY or UNIQUE constraints
    for (const auto& c : tbl.constraints) {
        if (c.type == ConstraintType::PrimaryKey || c.type == ConstraintType::Unique) {
            if (canonicalize_name(c.name) == canonical_idx ||
                canonicalize_name(c.enforcement_index_name) == canonical_idx) {
                return Status::constraint_violation(
                    "Cannot drop index '" + canonical_idx + "' because it enforces a " +
                    (c.type == ConstraintType::PrimaryKey ? "PRIMARY KEY" : "UNIQUE") + " constraint");
            }
        }
    }

    (void)ic_.drop_index(canonical_idx);
    Status drop_st = tc_.drop_index_from_table(tbl_name, canonical_idx);
    if (!drop_st.is_ok()) return drop_st;

    return QueryResult::make_message("Index '" + canonical_idx + "' dropped.");
}

Result<QueryResult> SqlExecutor::execute_insert(const InsertStatement& stmt) {
    auto tbl_res = tc_.get_table(stmt.table_name);
    if (tbl_res.is_err()) return tbl_res.status();
    const auto& tbl = tbl_res.value();

    std::vector<SqlValue> ordered_values;
    ordered_values.resize(tbl.columns.size());

    if (stmt.column_names.empty()) {
        // Positional insert
        if (stmt.values.size() != tbl.columns.size()) {
            return Status::syntax_error("Table '" + tbl.name + "' has " +
                                        std::to_string(tbl.columns.size()) + " columns, but " +
                                        std::to_string(stmt.values.size()) + " values were supplied");
        }
        for (size_t i = 0; i < tbl.columns.size(); ++i) {
            if (!stmt.values[i].is_null() && stmt.values[i].type() != tbl.columns[i].type) {
                return Status::type_mismatch("Column '" + tbl.columns[i].name + "' expects " +
                                             to_string(tbl.columns[i].type) + ", but got " +
                                             to_string(stmt.values[i].type()));
            }
            if (stmt.values[i].is_null()) {
                ordered_values[i] = SqlValue::null(tbl.columns[i].type);
            } else {
                ordered_values[i] = stmt.values[i];
            }
        }
    } else {
        // Named column insert
        if (stmt.column_names.size() != stmt.values.size()) {
            return Status::syntax_error("Number of column names (" +
                                        std::to_string(stmt.column_names.size()) +
                                        ") does not match number of values (" +
                                        std::to_string(stmt.values.size()) + ")");
        }

        // Initialize all columns with typed NULL
        for (size_t i = 0; i < tbl.columns.size(); ++i) {
            ordered_values[i] = SqlValue::null(tbl.columns[i].type);
        }

        std::unordered_set<size_t> assigned_indices;
        for (size_t i = 0; i < stmt.column_names.size(); ++i) {
            auto col_idx_opt = tbl.find_column_index(stmt.column_names[i]);
            if (!col_idx_opt.has_value()) {
                return Status::column_not_found("Column '" + stmt.column_names[i] +
                                                "' not found in table '" + tbl.name + "'");
            }
            size_t c_idx = *col_idx_opt;
            if (assigned_indices.contains(c_idx)) {
                return Status::duplicate_column("Column '" + tbl.columns[c_idx].name +
                                                "' specified multiple times in INSERT");
            }
            assigned_indices.insert(c_idx);

            if (!stmt.values[i].is_null() && stmt.values[i].type() != tbl.columns[c_idx].type) {
                return Status::type_mismatch("Column '" + tbl.columns[c_idx].name + "' expects " +
                                             to_string(tbl.columns[c_idx].type) + ", but got " +
                                             to_string(stmt.values[i].type()));
            }
            if (stmt.values[i].is_null()) {
                ordered_values[c_idx] = SqlValue::null(tbl.columns[c_idx].type);
            } else {
                ordered_values[c_idx] = stmt.values[i];
            }
        }
    }

    // Validate constraints (NOT NULL, PRIMARY KEY, UNIQUE, FOREIGN KEY)
    Status constr_st = validate_insert_constraints(tbl, ordered_values);
    if (!constr_st.is_ok()) {
        return constr_st;
    }

    // Encode row
    SqlRow row(std::move(ordered_values));
    auto encoded = row.encode(tbl.table_id);

    // Insert record into storage engine
    auto insert_res = rm_.insert_record(encoded);
    if (insert_res.is_err()) {
        return insert_res.status();
    }
    RecordId rid = insert_res.value();

    // Insert into indexes
    for (const auto& idx : tbl.indexes) {
        const auto& val = row.get(idx.column_index);
        if (val.is_null()) {
            continue; // Do not index NULL in unique or standard index
        }
        IndexKey key = val.to_index_key();
        BPlusTree tree(bpm_, idx.root_page_id);
        tree.set_on_root_change([this, &tbl, &idx](page_id_t new_root) {
            (void)tc_.update_index_root(tbl.name, idx.index_name, new_root);
            (void)ic_.update_root_page(idx.index_name, new_root);
        });

        Status ins_st = tree.insert(key, rid);
        if (!ins_st.is_ok()) {
            return ins_st;
        }

        if (tree.root_page_id() != idx.root_page_id) {
            (void)tc_.update_index_root(tbl.name, idx.index_name, tree.root_page_id());
            (void)ic_.update_root_page(idx.index_name, tree.root_page_id());
        }
    }

    if (bpm_.metrics() != nullptr) {
        bpm_.metrics()->record_rows_inserted(1);
    }

    return QueryResult::make_count(1, "Rows inserted");
}

Result<QueryResult> SqlExecutor::execute_explain(const ExplainStatement& stmt) {
    if (stmt.query == nullptr || stmt.query->type() != StatementType::Select) {
        return Status::unsupported_sql("EXPLAIN is only supported for read-only queries (SELECT)");
    }

    const auto& select_stmt = static_cast<const SelectStatement&>(*stmt.query);
    auto plan_start = std::chrono::steady_clock::now();
    auto plan_res = planner_.plan_select(select_stmt);
    auto plan_end = std::chrono::steady_clock::now();
    if (plan_res.is_err()) {
        return plan_res.status();
    }

    auto plan = std::move(plan_res.value());
    uint64_t plan_dur_us = std::chrono::duration_cast<std::chrono::microseconds>(plan_end - plan_start).count();
    plan.metrics.planning_time_us = plan_dur_us;

    std::string explain_str;
    if (stmt.is_analyze) {
        auto tbl_res = tc_.get_table(select_stmt.table_name);
        if (tbl_res.is_err()) return tbl_res.status();
        const auto& tbl = tbl_res.value();

        auto exec_start = std::chrono::steady_clock::now();
        auto rows_res = execute_plan_access_path(tbl, plan);
        if (rows_res.is_err()) return rows_res.status();
        auto matching_rows = std::move(rows_res.value());

        if (plan.order_by.has_value() && !plan.order_by_satisfied_by_index) {
            const auto& ob = plan.order_by.value();
            auto ob_idx_opt = tbl.find_column_index(ob.column_name);
            size_t ob_col_idx = *ob_idx_opt;
            bool asc = ob.ascending;
            std::stable_sort(matching_rows.begin(), matching_rows.end(),
                             [ob_col_idx, asc](const RowWithId& a, const RowWithId& b) {
                if (asc) return a.row.get(ob_col_idx) < b.row.get(ob_col_idx);
                return a.row.get(ob_col_idx) > b.row.get(ob_col_idx);
            });
        }

        if (plan.limit.has_value() && plan.limit->has_limit && !plan.limit_pushed_down) {
            if (matching_rows.size() > plan.limit->limit) {
                matching_rows.resize(plan.limit->limit);
            }
        }
        auto exec_end = std::chrono::steady_clock::now();
        uint64_t exec_dur_us = std::chrono::duration_cast<std::chrono::microseconds>(exec_end - exec_start).count();

        plan.metrics.rows_produced = matching_rows.size();
        plan.metrics.execution_time_us = exec_dur_us;
        explain_str = plan.explain_analyze_text();
    } else {
        explain_str = plan.explain_text();
    }

    std::vector<std::string> col_names = {stmt.is_analyze ? "QUERY PLAN & EXECUTION SUMMARY" : "QUERY PLAN"};
    std::vector<SqlType> col_types = {SqlType::Text};
    std::vector<SqlRow> rows;
    rows.emplace_back(std::vector<SqlValue>{SqlValue(std::move(explain_str))});

    return QueryResult::make_rows(std::move(col_names), std::move(col_types), std::move(rows));
}

Result<QueryResult> SqlExecutor::execute_analyze(const AnalyzeStatement& stmt) {
    if (stmt.table_name.empty()) {
        auto db_res = analyze_database(bpm_, rm_, tc_);
        if (db_res.is_err()) return db_res.status();
        std::string msg = "ANALYZE completed for " + std::to_string(db_res.value().size()) + " tables.";
        return QueryResult::make_message(msg);
    }

    auto tbl_res = tc_.get_table(stmt.table_name);
    if (tbl_res.is_err()) return tbl_res.status();
    const auto& tbl = tbl_res.value();

    auto a_res = analyze_table(bpm_, rm_, tbl);
    if (a_res.is_err()) return a_res.status();
    const auto& stats = a_res.value();

    std::string msg = "ANALYZE completed for table '" + tbl.name + "' (" +
                      std::to_string(stats.total_rows) + " rows, " +
                      std::to_string(stats.total_pages) + " pages).";
    return QueryResult::make_message(msg);
}

Result<QueryResult> SqlExecutor::execute_select(const SelectStatement& stmt) {
    auto tbl_res = tc_.get_table(stmt.table_name);
    if (tbl_res.is_err()) return tbl_res.status();
    const auto& tbl = tbl_res.value();

    // Determine projected columns
    std::vector<size_t> proj_indices;
    std::vector<std::string> proj_names;
    std::vector<SqlType> proj_types;

    if (stmt.select_all) {
        proj_indices.reserve(tbl.columns.size());
        proj_names.reserve(tbl.columns.size());
        proj_types.reserve(tbl.columns.size());
        for (size_t i = 0; i < tbl.columns.size(); ++i) {
            proj_indices.push_back(i);
            proj_names.push_back(tbl.columns[i].name);
            proj_types.push_back(tbl.columns[i].type);
        }
    } else {
        proj_indices.reserve(stmt.column_names.size());
        proj_names.reserve(stmt.column_names.size());
        proj_types.reserve(stmt.column_names.size());
        for (const auto& col_name : stmt.column_names) {
            auto idx_opt = tbl.find_column_index(col_name);
            if (!idx_opt.has_value()) {
                return Status::column_not_found("Column '" + col_name + "' not found in table '" +
                                                tbl.name + "'");
            }
            size_t c_idx = *idx_opt;
            proj_indices.push_back(c_idx);
            proj_names.push_back(tbl.columns[c_idx].name);
            proj_types.push_back(tbl.columns[c_idx].type);
        }
    }

    // Validate WHERE predicates
    for (const auto& pred : stmt.where.predicates) {
        auto idx_opt = tbl.find_column_index(pred.column_name);
        if (!idx_opt.has_value()) {
            return Status::column_not_found("Column '" + pred.column_name + "' in WHERE not found in table '" +
                                            tbl.name + "'");
        }
        if (!pred.value.is_null() && pred.value.type() != tbl.columns[*idx_opt].type) {
            return Status::type_mismatch("Type mismatch in WHERE comparison for column '" +
                                         pred.column_name + "'");
        }
    }

    // Validate ORDER BY if present
    if (stmt.order_by.has_value()) {
        auto ob_opt = tbl.find_column_index(stmt.order_by->column_name);
        if (!ob_opt.has_value()) {
            return Status::column_not_found("Column '" + stmt.order_by->column_name + "' in ORDER BY not found in table '" +
                                            tbl.name + "'");
        }
    }

    // 1. Generate optimized query execution plan
    auto plan_res = planner_.plan_select(stmt);
    if (plan_res.is_err()) return plan_res.status();
    auto plan = std::move(plan_res.value());

    // 2. Execute plan access path
    auto rows_res = execute_plan_access_path(tbl, plan);
    if (rows_res.is_err()) return rows_res.status();
    auto matching_rows = std::move(rows_res.value());

    // 3. Post-scan in-memory sort if index ordering did not satisfy ORDER BY
    if (plan.order_by.has_value() && !plan.order_by_satisfied_by_index) {
        const auto& ob = plan.order_by.value();
        auto ob_idx_opt = tbl.find_column_index(ob.column_name);
        size_t ob_col_idx = *ob_idx_opt;
        bool asc = ob.ascending;

        std::stable_sort(matching_rows.begin(), matching_rows.end(),
                         [ob_col_idx, asc](const RowWithId& a, const RowWithId& b) {
            if (asc) {
                return a.row.get(ob_col_idx) < b.row.get(ob_col_idx);
            }
            return a.row.get(ob_col_idx) > b.row.get(ob_col_idx);
        });
    }

    // 4. Post-sort LIMIT if limit was not pushed down
    if (plan.limit.has_value() && plan.limit->has_limit && !plan.limit_pushed_down) {
        if (matching_rows.size() > plan.limit->limit) {
            matching_rows.resize(plan.limit->limit);
        }
    }

    // 5. Project columns
    std::vector<SqlRow> result_rows;
    result_rows.reserve(matching_rows.size());

    for (const auto& item : matching_rows) {
        std::vector<SqlValue> projected_values;
        projected_values.reserve(proj_indices.size());
        for (size_t p_idx : proj_indices) {
            projected_values.push_back(item.row.get(p_idx));
        }
        result_rows.emplace_back(std::move(projected_values));
    }

    if (bpm_.metrics() != nullptr) {
        bpm_.metrics()->record_rows_examined(plan.metrics.rows_examined);
        bpm_.metrics()->record_rows_returned(result_rows.size());
        if (plan.order_by.has_value() && !plan.order_by_satisfied_by_index) {
            bpm_.metrics()->record_sort_operation();
        }
    }

    return QueryResult::make_rows(std::move(proj_names), std::move(proj_types), std::move(result_rows));
}

Result<std::vector<SqlExecutor::RowWithId>>
SqlExecutor::execute_plan_access_path(const TableSchema& schema, const QueryPlan& plan) {
    std::vector<RowWithId> results;
    auto expected_types = schema.column_types();

    const bool early_limit = plan.limit_pushed_down && plan.limit.has_value() && plan.limit->has_limit;
    const size_t limit_target = early_limit ? plan.limit->limit : std::numeric_limits<size_t>::max();

    if (plan.access_path == AccessPathType::IndexSeek) {
        if (bpm_.metrics() != nullptr) {
            bpm_.metrics()->record_index_seek();
        }
        if (!plan.index_access.has_value()) {
            return Status::unknown("IndexSeek plan missing index_access metadata");
        }
        BPlusTree tree(bpm_, plan.index_access->root_page_id);
        IndexKey key = plan.index_access->exact_value.to_index_key();
        auto rids_res = tree.find(key);
        if (rids_res.is_ok()) {
            for (const auto& rid : rids_res.value()) {
                plan.metrics.rows_examined++;
                auto rec_res = rm_.get_record(rid);
                if (rec_res.is_err()) continue;

                auto peek_res = SqlRow::peek_table_id(rec_res.value());
                if (peek_res.is_err() || peek_res.value() != schema.table_id) continue;

                auto row_res = SqlRow::decode(rec_res.value(), schema.table_id, expected_types);
                if (row_res.is_err()) continue;

                WhereClause res_where;
                res_where.predicates = plan.residual_filters;
                if (evaluate_where(schema, row_res.value(), res_where)) {
                    results.push_back(RowWithId{rid, std::move(row_res.value())});
                    plan.metrics.rows_produced++;
                    if (results.size() >= limit_target) break;
                }
            }
        }
        return results;
    }

    if (plan.access_path == AccessPathType::IndexRangeScan) {
        if (bpm_.metrics() != nullptr) {
            bpm_.metrics()->record_index_range_scan();
        }
        if (!plan.index_access.has_value()) {
            return Status::unknown("IndexRangeScan plan missing index_access metadata");
        }
        BPlusTree tree(bpm_, plan.index_access->root_page_id);

        std::optional<IndexKey> low_k;
        if (plan.index_access->low_value.has_value()) {
            low_k = plan.index_access->low_value->to_index_key();
        }

        std::optional<IndexKey> high_k;
        if (plan.index_access->high_value.has_value()) {
            high_k = plan.index_access->high_value->to_index_key();
        }

        auto entries_res = tree.scan_range(
            low_k,
            high_k,
            plan.index_access->inclusive_low,
            plan.index_access->inclusive_high
        );

        if (entries_res.is_ok()) {
            auto entries = std::move(entries_res.value());
            if (plan.scan_descending) {
                std::reverse(entries.begin(), entries.end());
            }

            for (const auto& entry : entries) {
                plan.metrics.rows_examined++;
                auto rec_res = rm_.get_record(entry.rid);
                if (rec_res.is_err()) continue;

                auto peek_res = SqlRow::peek_table_id(rec_res.value());
                if (peek_res.is_err() || peek_res.value() != schema.table_id) continue;

                auto row_res = SqlRow::decode(rec_res.value(), schema.table_id, expected_types);
                if (row_res.is_err()) continue;

                WhereClause res_where;
                res_where.predicates = plan.residual_filters;
                if (evaluate_where(schema, row_res.value(), res_where)) {
                    results.push_back(RowWithId{entry.rid, std::move(row_res.value())});
                    plan.metrics.rows_produced++;
                    if (results.size() >= limit_target) break;
                }
            }
        }
        return results;
    }

    // Fallback / Selected Table Scan
    if (bpm_.metrics() != nullptr) {
        bpm_.metrics()->record_table_scan();
    }
    const uint32_t total_pages = bpm_.page_file().page_count();
    for (page_id_t pid = 1; pid < total_pages; ++pid) {
        auto guard_res = bpm_.fetch_page_guard(pid);
        if (guard_res.is_err()) continue;

        auto& guard = guard_res.value();
        if (guard->page_type() != PageType::Record || !SlottedPage::is_slotted_page(*guard)) {
            continue;
        }

        SlottedPage slotted(*guard);
        const uint16_t slot_cnt = slotted.slot_count();

        for (slot_id_t slot = 0; slot < slot_cnt; ++slot) {
            auto span_res = slotted.get_record(slot);
            if (span_res.is_err()) continue;

            auto peek_res = SqlRow::peek_table_id(span_res.value());
            if (peek_res.is_err() || peek_res.value() != schema.table_id) continue;

            plan.metrics.rows_examined++;
            auto row_res = SqlRow::decode(span_res.value(), schema.table_id, expected_types);
            if (row_res.is_err()) continue;

            WhereClause full_where;
            full_where.predicates = plan.residual_filters;
            if (evaluate_where(schema, row_res.value(), full_where)) {
                results.push_back(RowWithId{RecordId{pid, slot}, std::move(row_res.value())});
                plan.metrics.rows_produced++;
                if (results.size() >= limit_target) break;
            }
        }
        if (results.size() >= limit_target) break;
    }

    return results;
}


Result<QueryResult> SqlExecutor::execute_update(const UpdateStatement& stmt) {
    if (stmt.where.empty()) {
        return Status::constraint_violation("UPDATE requires a WHERE clause for safety");
    }

    auto tbl_res = tc_.get_table(stmt.table_name);
    if (tbl_res.is_err()) return tbl_res.status();
    const auto& tbl = tbl_res.value();

    // Validate SET clauses
    std::vector<std::pair<size_t, SqlValue>> updates;
    std::unordered_set<size_t> updated_col_indices;

    for (const auto& set_cl : stmt.set_clauses) {
        auto col_opt = tbl.find_column_index(set_cl.column_name);
        if (!col_opt.has_value()) {
            return Status::column_not_found("Column '" + set_cl.column_name + "' in SET clause not found in table '" +
                                            tbl.name + "'");
        }
        size_t c_idx = *col_opt;
        if (updated_col_indices.contains(c_idx)) {
            return Status::duplicate_column("Column '" + tbl.columns[c_idx].name + "' updated multiple times");
        }
        updated_col_indices.insert(c_idx);

        if (!set_cl.value.is_null() && set_cl.value.type() != tbl.columns[c_idx].type) {
            return Status::type_mismatch("Column '" + tbl.columns[c_idx].name + "' expects " +
                                         to_string(tbl.columns[c_idx].type) + ", but got " +
                                         to_string(set_cl.value.type()));
        }
        if (set_cl.value.is_null()) {
            updates.emplace_back(c_idx, SqlValue::null(tbl.columns[c_idx].type));
        } else {
            updates.emplace_back(c_idx, set_cl.value);
        }
    }

    // Validate WHERE predicates
    for (const auto& pred : stmt.where.predicates) {
        auto idx_opt = tbl.find_column_index(pred.column_name);
        if (!idx_opt.has_value()) {
            return Status::column_not_found("Column '" + pred.column_name + "' in WHERE not found");
        }
        if (!pred.value.is_null() && pred.value.type() != tbl.columns[*idx_opt].type) {
            return Status::type_mismatch("Type mismatch in WHERE comparison for column '" +
                                         pred.column_name + "'");
        }
    }

    auto rows_res = find_matching_rows(tbl, stmt.where);
    if (rows_res.is_err()) return rows_res.status();
    auto rows = std::move(rows_res.value());

    // Pre-validate constraints before modifying any records
    Status constr_st = validate_update_constraints(tbl, rows, updates);
    if (!constr_st.is_ok()) {
        return constr_st;
    }

    size_t updated_count = 0;
    for (auto& item : rows) {
        // Delete old index entries for modified columns
        for (const auto& idx : tbl.indexes) {
            if (updated_col_indices.contains(idx.column_index)) {
                const auto& old_val = item.row.get(idx.column_index);
                if (!old_val.is_null()) {
                    IndexKey old_key = old_val.to_index_key();
                    BPlusTree tree(bpm_, idx.root_page_id);
                    (void)tree.delete_entry(old_key, item.rid);
                    if (tree.root_page_id() != idx.root_page_id) {
                        (void)tc_.update_index_root(tbl.name, idx.index_name, tree.root_page_id());
                        (void)ic_.update_root_page(idx.index_name, tree.root_page_id());
                    }
                }
            }
        }

        // Apply updates
        for (const auto& [c_idx, new_val] : updates) {
            item.row.set(c_idx, new_val);
        }

        // Insert new index entries for modified columns
        for (const auto& idx : tbl.indexes) {
            if (updated_col_indices.contains(idx.column_index)) {
                const auto& new_val = item.row.get(idx.column_index);
                if (!new_val.is_null()) {
                    IndexKey new_key = new_val.to_index_key();
                    BPlusTree tree(bpm_, idx.root_page_id);
                    tree.set_on_root_change([this, &tbl, &idx](page_id_t new_root) {
                        (void)tc_.update_index_root(tbl.name, idx.index_name, new_root);
                        (void)ic_.update_root_page(idx.index_name, new_root);
                    });
                    (void)tree.insert(new_key, item.rid);
                    if (tree.root_page_id() != idx.root_page_id) {
                        (void)tc_.update_index_root(tbl.name, idx.index_name, tree.root_page_id());
                        (void)ic_.update_root_page(idx.index_name, tree.root_page_id());
                    }
                }
            }
        }

        auto encoded = item.row.encode(tbl.table_id);
        Status upd_st = rm_.update_record(item.rid, encoded);
        if (!upd_st.is_ok()) {
            return upd_st;
        }
        updated_count++;
    }

    if (bpm_.metrics() != nullptr) {
        bpm_.metrics()->record_rows_updated(updated_count);
    }

    return QueryResult::make_count(updated_count, "Rows updated");
}

Result<QueryResult> SqlExecutor::execute_delete(const DeleteStatement& stmt) {
    if (stmt.where.empty()) {
        return Status::constraint_violation("DELETE requires a WHERE clause for safety");
    }

    auto tbl_res = tc_.get_table(stmt.table_name);
    if (tbl_res.is_err()) return tbl_res.status();
    const auto& tbl = tbl_res.value();

    // Validate WHERE predicates
    for (const auto& pred : stmt.where.predicates) {
        auto idx_opt = tbl.find_column_index(pred.column_name);
        if (!idx_opt.has_value()) {
            return Status::column_not_found("Column '" + pred.column_name + "' in WHERE not found");
        }
        if (!pred.value.is_null() && pred.value.type() != tbl.columns[*idx_opt].type) {
            return Status::type_mismatch("Type mismatch in WHERE comparison for column '" +
                                         pred.column_name + "'");
        }
    }

    auto rows_res = find_matching_rows(tbl, stmt.where);
    if (rows_res.is_err()) return rows_res.status();
    auto rows = std::move(rows_res.value());

    // Pre-validate DELETE constraints (FOREIGN KEY RESTRICT)
    Status constr_st = validate_delete_constraints(tbl, rows);
    if (!constr_st.is_ok()) {
        return constr_st;
    }

    size_t deleted_count = 0;
    for (const auto& item : rows) {
        // Delete from all indexes
        for (const auto& idx : tbl.indexes) {
            const auto& val = item.row.get(idx.column_index);
            if (!val.is_null()) {
                IndexKey key = val.to_index_key();
                BPlusTree tree(bpm_, idx.root_page_id);
                (void)tree.delete_entry(key, item.rid);
                if (tree.root_page_id() != idx.root_page_id) {
                    (void)tc_.update_index_root(tbl.name, idx.index_name, tree.root_page_id());
                    (void)ic_.update_root_page(idx.index_name, tree.root_page_id());
                }
            }
        }

        Status del_st = rm_.delete_record(item.rid);
        if (!del_st.is_ok()) {
            return del_st;
        }
        deleted_count++;
    }

    if (bpm_.metrics() != nullptr) {
        bpm_.metrics()->record_rows_deleted(deleted_count);
    }

    return QueryResult::make_count(deleted_count, "Rows deleted");
}

Status SqlExecutor::validate_insert_constraints(const TableSchema& tbl, const std::vector<SqlValue>& values) {
    // 1. NOT NULL constraint
    for (size_t i = 0; i < tbl.columns.size(); ++i) {
        if (tbl.is_not_null(tbl.columns[i].name) && values[i].is_null()) {
            return Status::constraint_violation("NOT NULL constraint violated for column '" +
                                                tbl.columns[i].name + "' in table '" + tbl.name + "'");
        }
    }

    // 2. PRIMARY KEY non-null constraint
    for (size_t i = 0; i < tbl.columns.size(); ++i) {
        if (tbl.is_primary_key(tbl.columns[i].name) && values[i].is_null()) {
            return Status::constraint_violation("PRIMARY KEY column '" +
                                                tbl.columns[i].name + "' cannot be NULL");
        }
    }

    // 3. FOREIGN KEY referential integrity check (child side)
    for (const auto* fk : tbl.find_foreign_keys()) {
        auto col_opt = tbl.find_column_index(fk->column_name);
        if (!col_opt.has_value()) continue;
        const auto& child_val = values[*col_opt];
        if (child_val.is_null()) {
            continue; // NULL allowed in FK
        }

        auto parent_tbl_res = tc_.get_table(fk->ref_table_name);
        if (parent_tbl_res.is_err()) {
            return Status::constraint_violation("Referenced parent table '" + fk->ref_table_name + "' not found");
        }
        const auto& parent_tbl = parent_tbl_res.value();

        WhereClause parent_where;
        parent_where.predicates.push_back(BinaryPredicate(fk->ref_column_name, BinaryOp::Equal, child_val));

        auto parent_rows_res = find_matching_rows(parent_tbl, parent_where);
        if (parent_rows_res.is_err() || parent_rows_res.value().empty()) {
            return Status::constraint_violation("FOREIGN KEY constraint '" + fk->name +
                                                "' violated: referenced row in '" + fk->ref_table_name +
                                                "' with " + fk->ref_column_name + "=" + child_val.to_string() + " does not exist");
        }
    }

    // 4. UNIQUE & PRIMARY KEY uniqueness check
    for (const auto& idx : tbl.indexes) {
        if (idx.is_unique && idx.root_page_id != INVALID_PAGE_ID) {
            const auto& val = values[idx.column_index];
            if (val.is_null()) {
                continue; // Multiple NULLs permitted in UNIQUE
            }
            IndexKey key = val.to_index_key();
            BPlusTree tree(bpm_, idx.root_page_id);
            auto find_res = tree.find(key);
            if (find_res.is_ok() && !find_res.value().empty()) {
                return Status::constraint_violation("Unique constraint violated for index '" +
                                                    idx.index_name + "' on key " + key.to_string());
            }
        }
    }

    return Status::ok();
}

Status SqlExecutor::validate_update_constraints(
    const TableSchema& tbl,
    const std::vector<RowWithId>& rows,
    const std::vector<std::pair<size_t, SqlValue>>& updates) {

    std::unordered_map<size_t, SqlValue> update_map;
    for (const auto& [c_idx, val] : updates) {
        update_map[c_idx] = val;
    }

    std::vector<std::vector<SqlValue>> candidate_rows;
    candidate_rows.reserve(rows.size());

    for (const auto& item : rows) {
        std::vector<SqlValue> cand = item.row.values();
        for (const auto& [c_idx, val] : update_map) {
            cand[c_idx] = val;
        }
        candidate_rows.push_back(std::move(cand));
    }

    // 1. NOT NULL & PRIMARY KEY non-null
    for (const auto& cand : candidate_rows) {
        for (size_t i = 0; i < tbl.columns.size(); ++i) {
            if (tbl.is_not_null(tbl.columns[i].name) && cand[i].is_null()) {
                return Status::constraint_violation("NOT NULL constraint violated for column '" +
                                                    tbl.columns[i].name + "' in table '" + tbl.name + "'");
            }
            if (tbl.is_primary_key(tbl.columns[i].name) && cand[i].is_null()) {
                return Status::constraint_violation("PRIMARY KEY column '" +
                                                    tbl.columns[i].name + "' cannot be NULL");
            }
        }
    }

    // 2. FOREIGN KEY checks (child side)
    for (const auto* fk : tbl.find_foreign_keys()) {
        auto col_opt = tbl.find_column_index(fk->column_name);
        if (!col_opt.has_value()) continue;
        size_t c_idx = *col_opt;
        if (!update_map.contains(c_idx)) continue;

        auto parent_tbl_res = tc_.get_table(fk->ref_table_name);
        if (parent_tbl_res.is_err()) {
            return Status::constraint_violation("Referenced parent table '" + fk->ref_table_name + "' not found");
        }
        const auto& parent_tbl = parent_tbl_res.value();

        for (const auto& cand : candidate_rows) {
            const auto& child_val = cand[c_idx];
            if (child_val.is_null()) continue;

            WhereClause parent_where;
            parent_where.predicates.push_back(BinaryPredicate(fk->ref_column_name, BinaryOp::Equal, child_val));

            auto parent_rows_res = find_matching_rows(parent_tbl, parent_where);
            if (parent_rows_res.is_err() || parent_rows_res.value().empty()) {
                return Status::constraint_violation("FOREIGN KEY constraint '" + fk->name +
                                                    "' violated: referenced row in '" + fk->ref_table_name +
                                                    "' with " + fk->ref_column_name + "=" + child_val.to_string() + " does not exist");
            }
        }
    }

    // 3. FOREIGN KEY RESTRICT (parent side)
    for (const auto& other_tbl : tc_.list_tables()) {
        for (const auto* fk : other_tbl.find_foreign_keys()) {
            if (canonicalize_name(fk->ref_table_name) != canonicalize_name(tbl.name)) continue;

            auto ref_col_opt = tbl.find_column_index(fk->ref_column_name);
            if (!ref_col_opt.has_value()) continue;
            size_t ref_c_idx = *ref_col_opt;

            if (!update_map.contains(ref_c_idx)) continue;

            for (size_t r = 0; r < rows.size(); ++r) {
                const auto& old_val = rows[r].row.get(ref_c_idx);
                const auto& new_val = candidate_rows[r][ref_c_idx];
                if (old_val == new_val) continue;

                WhereClause child_where;
                child_where.predicates.push_back(BinaryPredicate(fk->column_name, BinaryOp::Equal, old_val));
                auto child_rows_res = find_matching_rows(other_tbl, child_where);
                if (child_rows_res.is_ok() && !child_rows_res.value().empty()) {
                    return Status::constraint_violation(
                        "FOREIGN KEY constraint '" + fk->name +
                        "' violated: cannot update referenced parent row key in '" + tbl.name +
                        "' because referencing rows exist in '" + other_tbl.name + "' (RESTRICT)");
                }
            }
        }
    }

    // 4. UNIQUE & PRIMARY KEY uniqueness
    for (const auto& idx : tbl.indexes) {
        if (!idx.is_unique || idx.root_page_id == INVALID_PAGE_ID) continue;
        if (!update_map.contains(idx.column_index)) continue;

        std::unordered_set<std::string> batch_seen;

        for (size_t r = 0; r < rows.size(); ++r) {
            const auto& new_val = candidate_rows[r][idx.column_index];
            if (new_val.is_null()) continue;

            std::string key_str = new_val.to_string();
            if (batch_seen.contains(key_str)) {
                return Status::constraint_violation("Unique constraint violated for index '" +
                                                    idx.index_name + "' on key " + key_str +
                                                    " (duplicate in update batch)");
            }
            batch_seen.insert(key_str);

            IndexKey key = new_val.to_index_key();
            BPlusTree tree(bpm_, idx.root_page_id);
            auto find_res = tree.find(key);
            if (find_res.is_ok()) {
                for (const auto& existing_rid : find_res.value()) {
                    if (existing_rid != rows[r].rid) {
                        return Status::constraint_violation("Unique constraint violated for index '" +
                                                            idx.index_name + "' on key " + key.to_string());
                    }
                }
            }
        }
    }

    return Status::ok();
}

Status SqlExecutor::validate_delete_constraints(
    const TableSchema& tbl,
    const std::vector<RowWithId>& rows) {

    for (const auto& other_tbl : tc_.list_tables()) {
        for (const auto* fk : other_tbl.find_foreign_keys()) {
            if (canonicalize_name(fk->ref_table_name) != canonicalize_name(tbl.name)) continue;

            auto ref_col_opt = tbl.find_column_index(fk->ref_column_name);
            if (!ref_col_opt.has_value()) continue;
            size_t ref_c_idx = *ref_col_opt;

            const bool is_self_ref = (canonicalize_name(other_tbl.name) == canonicalize_name(tbl.name));
            std::unordered_set<RecordId> deleting_rids;
            if (is_self_ref) {
                for (const auto& item : rows) {
                    deleting_rids.insert(item.rid);
                }
            }

            for (const auto& item : rows) {
                const auto& parent_val = item.row.get(ref_c_idx);
                if (parent_val.is_null()) continue;

                WhereClause child_where;
                child_where.predicates.push_back(BinaryPredicate(fk->column_name, BinaryOp::Equal, parent_val));

                auto child_rows_res = find_matching_rows(other_tbl, child_where);
                if (child_rows_res.is_ok()) {
                    for (const auto& child_item : child_rows_res.value()) {
                        if (is_self_ref && deleting_rids.contains(child_item.rid)) {
                            continue;
                        }
                        return Status::constraint_violation(
                            "FOREIGN KEY constraint '" + fk->name +
                            "' violated: cannot delete referenced parent row from '" + tbl.name +
                            "' because referencing rows exist in '" + other_tbl.name + "' (RESTRICT)");
                    }
                }
            }
        }
    }

    return Status::ok();
}

Result<std::vector<SqlExecutor::RowWithId>>
SqlExecutor::find_matching_rows(const TableSchema& schema, const WhereClause& where) {
    std::vector<RowWithId> results;
    auto expected_types = schema.column_types();

    // 1. Try to find an index lookup candidate: equality on an indexed column
    const TableIndexInfo* usable_index = nullptr;
    const BinaryPredicate* eq_pred = nullptr;

    for (const auto& pred : where.predicates) {
        if (pred.op == BinaryOp::Equal) {
            auto idx = schema.find_index_for_column(pred.column_name);
            if (idx != nullptr && idx->root_page_id != INVALID_PAGE_ID) {
                usable_index = idx;
                eq_pred = &pred;
                break;
            }
        }
    }

    if (usable_index != nullptr && eq_pred != nullptr && !eq_pred->value.is_null()) {
        // Fast path: Indexed lookup
        BPlusTree tree(bpm_, usable_index->root_page_id);
        IndexKey key = eq_pred->value.to_index_key();
        auto rids_res = tree.find(key);

        if (rids_res.is_ok()) {
            for (const auto& rid : rids_res.value()) {
                auto rec_res = rm_.get_record(rid);
                if (rec_res.is_err()) continue;

                auto peek_res = SqlRow::peek_table_id(rec_res.value());
                if (peek_res.is_err() || peek_res.value() != schema.table_id) continue;

                auto row_res = SqlRow::decode(rec_res.value(), schema.table_id, expected_types);
                if (row_res.is_err()) continue;

                if (evaluate_where(schema, row_res.value(), where)) {
                    results.push_back(RowWithId{rid, std::move(row_res.value())});
                }
            }
        }
        return results;
    }

    // 2. Fallback path: Full Table Scan
    const uint32_t total_pages = bpm_.page_file().page_count();
    for (page_id_t pid = 1; pid < total_pages; ++pid) {
        auto guard_res = bpm_.fetch_page_guard(pid);
        if (guard_res.is_err()) {
            continue;
        }

        auto& guard = guard_res.value();
        if (guard->page_type() != PageType::Record || !SlottedPage::is_slotted_page(*guard)) {
            continue;
        }

        SlottedPage slotted(*guard);
        const uint16_t slot_cnt = slotted.slot_count();

        for (slot_id_t slot = 0; slot < slot_cnt; ++slot) {
            auto span_res = slotted.get_record(slot);
            if (span_res.is_err()) {
                continue; // deleted or invalid slot
            }

            auto peek_res = SqlRow::peek_table_id(span_res.value());
            if (peek_res.is_err() || peek_res.value() != schema.table_id) {
                continue;
            }

            auto row_res = SqlRow::decode(span_res.value(), schema.table_id, expected_types);
            if (row_res.is_err()) {
                continue;
            }

            if (evaluate_where(schema, row_res.value(), where)) {
                results.push_back(RowWithId{RecordId{pid, slot}, std::move(row_res.value())});
            }
        }
    }

    return results;
}

bool SqlExecutor::evaluate_where(const TableSchema& schema,
                                 const SqlRow& row,
                                 const WhereClause& where) const {
    if (where.empty()) return true;

    for (const auto& pred : where.predicates) {
        auto c_opt = schema.find_column_index(pred.column_name);
        if (!c_opt.has_value()) return false;
        size_t c_idx = *c_opt;

        const auto& cell_val = row.get(c_idx);
        if (!pred.evaluate(cell_val)) {
            return false;
        }
    }

    return true;
}

} // namespace klyrdb
