#pragma once

#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/storage/record_manager.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/index/bplus_tree.hpp"
#include "klyrdb/sql/ast.hpp"
#include "klyrdb/sql/query_result.hpp"
#include "klyrdb/sql/query_planner.hpp"
#include "klyrdb/concurrency/database_lock.hpp"
#include "klyrdb/concurrency/lock_manager.hpp"
#include <string_view>
#include <memory>
#include <chrono>

namespace klyrdb {

class TransactionManager;

/// Query Execution Engine consuming parsed AST statements and coordinating
/// TableCatalog, RecordManager, IndexCatalog, BPlusTree, and BufferPoolManager.
class SqlExecutor {
public:
    SqlExecutor(BufferPoolManager& bpm,
                RecordManager& rm,
                TableCatalog& tc,
                IndexCatalog& ic) noexcept
        : bpm_(bpm), rm_(rm), tc_(tc), ic_(ic), planner_(bpm, tc, ic),
          db_lock_(LockManager::instance().get_lock(bpm.page_file().path(), false)) {}

    ~SqlExecutor();

    /// Tokenizes, parses, and executes a SQL statement.
    Result<QueryResult> execute(std::string_view sql);

    /// Executes a pre-parsed AST statement.
    Result<QueryResult> execute(const Statement& stmt);

    void set_transaction_manager(TransactionManager* tm) noexcept { txn_mgr_ = tm; }
    [[nodiscard]] TransactionManager* transaction_manager() const noexcept { return txn_mgr_; }

    void set_database_lock(std::shared_ptr<DatabaseLock> lock) noexcept { db_lock_ = std::move(lock); }
    [[nodiscard]] std::shared_ptr<DatabaseLock> database_lock() const noexcept { return db_lock_; }

    void set_lock_timeout(std::chrono::milliseconds timeout) noexcept { lock_timeout_ = timeout; }
    [[nodiscard]] std::chrono::milliseconds lock_timeout() const noexcept { return lock_timeout_; }

    [[nodiscard]] BufferPoolManager& buffer_pool_manager() noexcept { return bpm_; }
    [[nodiscard]] RecordManager& record_manager() noexcept { return rm_; }
    [[nodiscard]] TableCatalog& table_catalog() noexcept { return tc_; }
    [[nodiscard]] IndexCatalog& index_catalog() noexcept { return ic_; }
    [[nodiscard]] QueryPlanner& query_planner() noexcept { return planner_; }

private:
    Result<QueryResult> execute_internal(const Statement& stmt);
    Result<QueryResult> execute_create_table(const CreateTableStatement& stmt);
    Result<QueryResult> execute_create_index(const CreateIndexStatement& stmt);
    Result<QueryResult> execute_drop_table(const DropTableStatement& stmt);
    Result<QueryResult> execute_drop_index(const DropIndexStatement& stmt);
    Result<QueryResult> execute_insert(const InsertStatement& stmt);
    Result<QueryResult> execute_select(const SelectStatement& stmt);
    Result<QueryResult> execute_update(const UpdateStatement& stmt);
    Result<QueryResult> execute_delete(const DeleteStatement& stmt);
    Result<QueryResult> execute_explain(const ExplainStatement& stmt);
    Result<QueryResult> execute_analyze(const AnalyzeStatement& stmt);

    struct RowWithId {
        RecordId rid;
        SqlRow row;
    };

    Result<std::vector<RowWithId>> find_matching_rows(const TableSchema& schema,
                                                      const WhereClause& where);

    Result<std::vector<RowWithId>> execute_plan_access_path(const TableSchema& schema,
                                                            const QueryPlan& plan);

    bool evaluate_where(const TableSchema& schema,
                        const SqlRow& row,
                        const WhereClause& where) const;

    Status validate_insert_constraints(const TableSchema& tbl, const std::vector<SqlValue>& values);
    Status validate_update_constraints(const TableSchema& tbl,
                                       const std::vector<RowWithId>& rows,
                                       const std::vector<std::pair<size_t, SqlValue>>& updates);
    Status validate_delete_constraints(const TableSchema& tbl,
                                       const std::vector<RowWithId>& rows);

    BufferPoolManager& bpm_;
    RecordManager& rm_;
    TableCatalog& tc_;
    IndexCatalog& ic_;
    QueryPlanner planner_;
    TransactionManager* txn_mgr_{nullptr};
    std::shared_ptr<DatabaseLock> db_lock_{nullptr};
    std::chrono::milliseconds lock_timeout_{5000};
    bool owns_explicit_txn_lock_{false};
};

} // namespace klyrdb
