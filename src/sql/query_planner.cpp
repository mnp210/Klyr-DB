#include "klyrdb/sql/query_planner.hpp"
#include "klyrdb/storage/slotted_page.hpp"
#include "klyrdb/storage/page_constants.hpp"
#include "klyrdb/catalog/table_statistics.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_set>

namespace klyrdb {

namespace {

constexpr double COST_SCAN_STARTUP = 0.5;
constexpr double COST_PAGE_IO      = 1.0;
constexpr double COST_RANDOM_IO    = 0.1;
constexpr double COST_CPU_TUPLE    = 0.01;
constexpr double COST_CPU_SORT     = 0.05;

} // namespace

size_t QueryPlanner::estimate_table_rows(const TableSchema& schema) {
    auto stats_opt = TableStatisticsRegistry::instance().get_statistics(bpm_.page_file().path().string(), schema.table_id);
    if (stats_opt.has_value() && stats_opt->total_rows > 0) {
        return stats_opt->total_rows;
    }

    size_t count = 0;
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
            if (peek_res.is_ok() && peek_res.value() == schema.table_id) {
                count++;
            }
        }
    }

    return std::max<size_t>(1, count);
}

QueryPlanner::CandidatePlan QueryPlanner::evaluate_table_scan(
    const TableSchema& schema,
    size_t total_rows,
    size_t total_pages,
    const SelectStatement& stmt) {
    (void)schema;

    CandidatePlan plan;
    plan.access_path = AccessPathType::TableScan;

    // Estimate selectivity of WHERE predicates
    double selectivity = 1.0;
    for (const auto& pred : stmt.where.predicates) {
        if (pred.value.is_null()) {
            selectivity *= 0.05;
        } else if (pred.op == BinaryOp::Equal) {
            selectivity *= 0.10;
        } else if (pred.op == BinaryOp::NotEqual) {
            selectivity *= 0.90;
        } else {
            // Range comparison: <, <=, >, >=
            selectivity *= 0.35;
        }
    }

    size_t filtered_rows = std::max<size_t>(1, static_cast<size_t>(std::round(total_rows * selectivity)));

    double cost = COST_SCAN_STARTUP + (total_pages * COST_PAGE_IO) + (total_rows * COST_CPU_TUPLE);

    if (stmt.order_by.has_value()) {
        plan.order_by_satisfied = false;
        cost += filtered_rows * std::log2(std::max<double>(2.0, static_cast<double>(filtered_rows))) * COST_CPU_SORT;
    } else {
        plan.order_by_satisfied = true;
    }

    plan.cost.estimated_rows = filtered_rows;
    plan.cost.estimated_cost = cost;
    plan.cost.estimated_pages = total_pages;

    return plan;
}

std::vector<QueryPlanner::CandidatePlan> QueryPlanner::evaluate_index_candidates(
    const TableSchema& schema,
    size_t total_rows,
    size_t total_pages,
    const SelectStatement& stmt) {
    (void)total_pages;

    std::vector<CandidatePlan> candidates;

    for (const auto& idx : schema.indexes) {
        page_id_t live_root = idx.root_page_id;
        auto idx_res = ic_.get_index(idx.index_name);
        if (idx_res.is_ok()) {
            live_root = idx_res.value().root_page_id;
        }
        std::string canonical_col = canonicalize_name(idx.column_name);

        // 1. Check for Equality Predicate on this index
        for (size_t p_idx = 0; p_idx < stmt.where.predicates.size(); ++p_idx) {
            const auto& pred = stmt.where.predicates[p_idx];
            if (canonicalize_name(pred.column_name) == canonical_col &&
                pred.op == BinaryOp::Equal &&
                !pred.value.is_null()) {

                CandidatePlan plan;
                plan.access_path = AccessPathType::IndexSeek;
                plan.consumed_predicate_indices.push_back(p_idx);

                IndexAccessPredicate acc;
                acc.column_name = idx.column_name;
                acc.index_name = idx.index_name;
                acc.root_page_id = live_root;
                acc.is_unique = idx.is_unique;
                acc.is_range = false;
                acc.exact_value = pred.value;
                plan.index_access = acc;

                size_t est_rows = idx.is_unique ? 1 : std::max<size_t>(1, total_rows / 10);
                double cost = (0.5 * COST_PAGE_IO) + (est_rows * COST_RANDOM_IO);

                const size_t residual_cnt = stmt.where.predicates.size() - 1;
                cost += est_rows * residual_cnt * COST_CPU_TUPLE;

                // An equality lookup produces 1 row or identical key values, which trivially satisfies ORDER BY
                plan.order_by_satisfied = true;

                plan.cost.estimated_rows = est_rows;
                plan.cost.estimated_cost = cost;
                plan.cost.estimated_pages = 1 + est_rows;

                candidates.push_back(std::move(plan));
                break;
            }
        }

        // 2. Check for Range Predicates on this index (<, <=, >, >=)
        std::vector<size_t> range_pred_indices;
        std::optional<SqlValue> low_val;
        bool inc_low = true;
        std::optional<SqlValue> high_val;
        bool inc_high = true;

        for (size_t p_idx = 0; p_idx < stmt.where.predicates.size(); ++p_idx) {
            const auto& pred = stmt.where.predicates[p_idx];
            if (canonicalize_name(pred.column_name) != canonical_col || pred.value.is_null()) {
                continue;
            }

            if (pred.op == BinaryOp::GreaterThan) {
                range_pred_indices.push_back(p_idx);
                if (!low_val.has_value() || pred.value >= *low_val) {
                    low_val = pred.value;
                    inc_low = false;
                }
            } else if (pred.op == BinaryOp::GreaterEqual) {
                range_pred_indices.push_back(p_idx);
                if (!low_val.has_value() || pred.value > *low_val) {
                    low_val = pred.value;
                    inc_low = true;
                }
            } else if (pred.op == BinaryOp::LessThan) {
                range_pred_indices.push_back(p_idx);
                if (!high_val.has_value() || pred.value <= *high_val) {
                    high_val = pred.value;
                    inc_high = false;
                }
            } else if (pred.op == BinaryOp::LessEqual) {
                range_pred_indices.push_back(p_idx);
                if (!high_val.has_value() || pred.value < *high_val) {
                    high_val = pred.value;
                    inc_high = true;
                }
            }
        }

        if (!range_pred_indices.empty()) {
            CandidatePlan plan;
            plan.access_path = AccessPathType::IndexRangeScan;
            plan.consumed_predicate_indices = range_pred_indices;

            IndexAccessPredicate acc;
            acc.column_name = idx.column_name;
            acc.index_name = idx.index_name;
            acc.root_page_id = live_root;
            acc.is_unique = idx.is_unique;
            acc.is_range = true;
            acc.low_value = low_val;
            acc.inclusive_low = inc_low;
            acc.high_value = high_val;
            acc.inclusive_high = inc_high;
            plan.index_access = acc;

            double selectivity = 0.35; // single-bounded default
            if (low_val.has_value() && high_val.has_value()) {
                selectivity = 0.15; // double-bounded default
            }

            size_t est_rows = std::max<size_t>(1, static_cast<size_t>(std::round(total_rows * selectivity)));
            double cost = (0.8 * COST_PAGE_IO) + (est_rows * COST_RANDOM_IO);

            const size_t residual_cnt = stmt.where.predicates.size() - range_pred_indices.size();
            cost += est_rows * residual_cnt * COST_CPU_TUPLE;

            // Check if index matches ORDER BY
            if (stmt.order_by.has_value()) {
                if (canonicalize_name(stmt.order_by->column_name) == canonical_col) {
                    plan.order_by_satisfied = true;
                    plan.scan_descending = !stmt.order_by->ascending;
                } else {
                    plan.order_by_satisfied = false;
                    cost += est_rows * std::log2(std::max<double>(2.0, static_cast<double>(est_rows))) * COST_CPU_SORT;
                }
            } else {
                plan.order_by_satisfied = true;
            }

            plan.cost.estimated_rows = est_rows;
            plan.cost.estimated_cost = cost;
            plan.cost.estimated_pages = 3 + est_rows;

            candidates.push_back(std::move(plan));
        }

        // 3. Check for ORDER BY satisfaction without WHERE (or when index provides ordering)
        if (stmt.order_by.has_value() && canonicalize_name(stmt.order_by->column_name) == canonical_col) {
            CandidatePlan plan;
            plan.access_path = AccessPathType::IndexRangeScan;

            IndexAccessPredicate acc;
            acc.column_name = idx.column_name;
            acc.index_name = idx.index_name;
            acc.root_page_id = live_root;
            acc.is_unique = idx.is_unique;
            acc.is_range = true;
            plan.index_access = acc;

            plan.order_by_satisfied = true;
            plan.scan_descending = !stmt.order_by->ascending;

            size_t est_rows = total_rows;
            if (stmt.limit.has_limit && stmt.where.empty()) {
                est_rows = std::min(stmt.limit.limit, total_rows);
            }

            double cost = (0.8 * COST_PAGE_IO) + (est_rows * COST_RANDOM_IO);
            cost += est_rows * stmt.where.predicates.size() * COST_CPU_TUPLE;

            plan.cost.estimated_rows = est_rows;
            plan.cost.estimated_cost = cost;
            plan.cost.estimated_pages = 3 + est_rows;

            candidates.push_back(std::move(plan));
        }
    }

    return candidates;
}

Result<QueryPlan> QueryPlanner::plan_select(const SelectStatement& stmt) {
    auto tbl_res = tc_.get_table(stmt.table_name);
    if (tbl_res.is_err()) return tbl_res.status();
    const auto& schema = tbl_res.value();

    const size_t total_rows = estimate_table_rows(schema);
    const size_t total_pages = std::max<size_t>(1, (total_rows * 64) / MAX_PAYLOAD_SIZE);

    // 1. Evaluate baseline Table Scan
    CandidatePlan best_plan = evaluate_table_scan(schema, total_rows, total_pages, stmt);

    // 2. Evaluate all available Index candidates
    auto index_candidates = evaluate_index_candidates(schema, total_rows, total_pages, stmt);

    // 3. Choose candidate with lowest estimated cost
    for (auto& cand : index_candidates) {
        if (cand.cost.estimated_cost < best_plan.cost.estimated_cost) {
            best_plan = std::move(cand);
        }
    }

    // 4. Assemble final QueryPlan
    QueryPlan plan;
    plan.table_name = schema.name;
    plan.table_id = schema.table_id;
    plan.access_path = best_plan.access_path;
    plan.index_access = best_plan.index_access;
    plan.order_by = stmt.order_by;
    plan.order_by_satisfied_by_index = best_plan.order_by_satisfied;
    plan.scan_descending = best_plan.scan_descending;
    plan.cost = best_plan.cost;

    // Collect residual filters (all predicates not consumed by index access)
    std::unordered_set<size_t> consumed(best_plan.consumed_predicate_indices.begin(),
                                        best_plan.consumed_predicate_indices.end());
    for (size_t i = 0; i < stmt.where.predicates.size(); ++i) {
        if (!consumed.contains(i)) {
            plan.residual_filters.push_back(stmt.where.predicates[i]);
        }
    }

    // Projection
    plan.select_all = stmt.select_all;
    if (stmt.select_all) {
        for (const auto& col : schema.columns) {
            plan.projected_columns.push_back(col.name);
        }
    } else {
        plan.projected_columns = stmt.column_names;
    }

    // LIMIT handling & pushdown
    plan.limit = stmt.limit;
    if (stmt.limit.has_limit) {
        if (best_plan.order_by_satisfied || !stmt.order_by.has_value()) {
            plan.limit_pushed_down = true;
        } else {
            plan.limit_pushed_down = false;
        }
    }

    return plan;
}

std::string QueryPlan::explain_text() const {
    std::string out;
    out += "Query Plan\n";
    out += "----------\n";
    out += to_string(access_path) + "\n";
    out += "  Table: " + table_name + "\n";
    if (index_access.has_value()) {
        out += "  Index: " + index_access->index_name + "\n";
        out += "  Predicate: " + index_access->predicate_string() + "\n";
    }
    if (!residual_filters.empty()) {
        out += "  Residual Filter: ";
        for (size_t i = 0; i < residual_filters.size(); ++i) {
            if (i > 0) out += " AND ";
            out += residual_filters[i].column_name + " "
                 + to_string(residual_filters[i].op) + " "
                 + residual_filters[i].value.to_string();
        }
        out += "\n";
    }
    if (order_by.has_value()) {
        out += "  Order: " + order_by->column_name + (order_by->ascending ? " ASC" : " DESC");
        if (order_by_satisfied_by_index) {
            out += std::string(" (Satisfied by index") + (scan_descending ? " [Reverse Scan]" : "") + ")\n";
        } else {
            out += " (Explicit Sort)\n";
        }
    }
    if (limit.has_value() && limit->has_limit) {
        out += "  Limit: " + std::to_string(limit->limit);
        if (limit_pushed_down) {
            out += " (Pushed down)\n";
        } else {
            out += " (Post-sort)\n";
        }
    }
    out += "  Estimated Rows: " + std::to_string(cost.estimated_rows) + "\n";
    char cost_buf[32];
    std::snprintf(cost_buf, sizeof(cost_buf), "%.2f", cost.estimated_cost);
    out += "  Estimated Cost: " + std::string(cost_buf) + "\n";
    return out;
}

std::string QueryPlan::explain_analyze_text() const {
    std::string out = explain_text();
    out += "\nExecution Summary\n";
    out += "-----------------\n";
    out += "  Actual Rows:   " + std::to_string(metrics.rows_produced) + "\n";
    out += "  Rows Examined: " + std::to_string(metrics.rows_examined) + "\n";
    out += "  Planning Time: " + std::to_string(metrics.planning_time_us) + " us\n";
    out += "  Exec Time:     " + std::to_string(metrics.execution_time_us) + " us\n";
    return out;
}

} // namespace klyrdb
