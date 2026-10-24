#pragma once

#include "klyrdb/common/status.hpp"
#include "klyrdb/common/result.hpp"
#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/index/index_catalog.hpp"
#include "klyrdb/storage/buffer_pool_manager.hpp"
#include "klyrdb/sql/ast.hpp"
#include "klyrdb/sql/query_plan.hpp"

namespace klyrdb {

/// Query Planner responsible for cost-based access path selection,
/// predicate analysis, index seek/range scan matching, ORDER BY satisfaction,
/// and LIMIT pushdown.
class QueryPlanner {
public:
    QueryPlanner(BufferPoolManager& bpm,
                 TableCatalog& tc,
                 IndexCatalog& ic) noexcept
        : bpm_(bpm), tc_(tc), ic_(ic) {}

    /// Produces an optimized QueryPlan from a parsed SelectStatement.
    [[nodiscard]] Result<QueryPlan> plan_select(const SelectStatement& stmt);

    /// Fast estimation of total row count for a table.
    [[nodiscard]] size_t estimate_table_rows(const TableSchema& schema);

private:
    struct CandidatePlan {
        AccessPathType access_path;
        std::optional<IndexAccessPredicate> index_access;
        std::vector<size_t> consumed_predicate_indices;
        bool order_by_satisfied{false};
        bool scan_descending{false};
        PlanCost cost;
    };

    CandidatePlan evaluate_table_scan(
        const TableSchema& schema,
        size_t total_rows,
        size_t total_pages,
        const SelectStatement& stmt);

    std::vector<CandidatePlan> evaluate_index_candidates(
        const TableSchema& schema,
        size_t total_rows,
        size_t total_pages,
        const SelectStatement& stmt);

    BufferPoolManager& bpm_;
    TableCatalog& tc_;
    IndexCatalog& ic_;
};

} // namespace klyrdb
