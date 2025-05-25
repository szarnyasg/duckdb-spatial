#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/column_lifetime_analyzer.hpp"
#include "duckdb/optimizer/matcher/expression_matcher.hpp"
#include "duckdb/optimizer/matcher/function_matcher.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/optimizer/remove_unused_columns.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator_extension.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/main/database.hpp"

#include "spatial/geometry/bbox.hpp"
#include "spatial/geometry/geometry_type.hpp"
#include "spatial/index/rtree/rtree_index.hpp"
#include "spatial/index/rtree/rtree_index_create_logical.hpp"
#include "spatial/index/rtree/rtree_index_scan.hpp"
#include "spatial/index/rtree/rtree_module.hpp"
#include "spatial/spatial_types.hpp"
#include "spatial/util/math.hpp"

namespace duckdb {
//-----------------------------------------------------------------------------
// Plan rewriter
//-----------------------------------------------------------------------------
class RTreeIndexScanOptimizer : public OptimizerExtension {
public:
	RTreeIndexScanOptimizer() {
		optimize_function = RTreeIndexScanOptimizer::Optimize;
	}

	static void RewriteIndexExpression(Index &index, LogicalGet &get, Expression &expr, bool &rewrite_possible) {
		if (expr.type == ExpressionType::BOUND_COLUMN_REF) {
			auto &bound_colref = expr.Cast<BoundColumnRefExpression>();
			// bound column ref: rewrite to fit in the current set of bound column ids
			bound_colref.binding.table_index = get.table_index;
			auto &column_ids = index.GetColumnIds();
			auto &get_column_ids = get.GetColumnIds();
			column_t referenced_column = column_ids[bound_colref.binding.column_index];
			// search for the referenced column in the set of column_ids
			for (idx_t i = 0; i < get_column_ids.size(); i++) {
				if (get_column_ids[i].GetPrimaryIndex() == referenced_column) {
					bound_colref.binding.column_index = i;
					return;
				}
			}
			// column id not found in bound columns in the LogicalGet: rewrite not possible
			rewrite_possible = false;
		}
		ExpressionIterator::EnumerateChildren(
		    expr, [&](Expression &child) { RewriteIndexExpression(index, get, child, rewrite_possible); });
	}

	static void RewriteIndexExpressionForFilter(Index &index, LogicalGet &get, unique_ptr<Expression> &expr,
	                                            idx_t filter_idx, bool &rewrite_possible) {
		if (expr->type == ExpressionType::BOUND_COLUMN_REF) {
			auto &bound_colref = expr->Cast<BoundColumnRefExpression>();
			if (bound_colref.binding.column_index != filter_idx) {
				rewrite_possible = false;
				return;
			}
			// this column matches the index column - turn it into a BoundReference
			expr = make_uniq<BoundReferenceExpression>(bound_colref.return_type, 0ULL);
			return;
		}
		ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
			RewriteIndexExpressionForFilter(index, get, child, filter_idx, rewrite_possible);
		});
	}

	static bool IsSpatialPredicate(const ScalarFunction &function, const unordered_set<string> &predicates) {

		if (predicates.find(function.name) == predicates.end()) {
			return false;
		}
		if (function.arguments.size() < 2) {
			// We can only optimize if there are two children
			return false;
		}
		if (function.arguments[0] != GeoTypes::GEOMETRY()) {
			// We can only optimize if the first child is a GEOMETRY
			return false;
		}
		if (function.arguments[1] != GeoTypes::GEOMETRY()) {
			// We can only optimize if the second child is a GEOMETRY
			return false;
		}
		if (function.return_type != LogicalType::BOOLEAN) {
			// We can only optimize if the return type is a BOOLEAN
			return false;
		}
		return true;
	}

	static bool TryGetBoundingBox(const Value &value, Box2D<float> &bbox) {
		const auto str = value.GetValueUnsafe<string_t>();
		const geometry_t blob(str);
		if (!blob.TryGetCachedBounds(bbox)) {
			return false;
		}
		return true;
	}

	static bool TryOptimize(Binder &binder, ClientContext &context, unique_ptr<LogicalOperator> &plan,
	                        unique_ptr<LogicalOperator> &root) {
		// Look for a FILTER with a spatial predicate followed by a LOGICAL_GET table scan
		// OR for a seq_scan with an ExpressionFilter
		auto &op = *plan;

		if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
			// extract the filter from the filter node
			// Look for a spatial predicate
			auto &filter = op.Cast<LogicalFilter>();

			if (filter.expressions.size() != 1) {
				// We can only optimize if there is a single expression right now
				return false;
			}
			auto &filter_expr = filter.expressions[0];
			// Look for a table scan
			if (filter.children.front()->type != LogicalOperatorType::LOGICAL_GET) {
				return false;
			}
			auto &get_ptr = filter.children.front();
			return TryOptimizeGet(binder, context, get_ptr, root, filter, optional_idx(), filter_expr);
		}
		if (op.type == LogicalOperatorType::LOGICAL_GET) {
			// this is a LogicalGet - check if there is an ExpressionFilter
			auto &get = op.Cast<LogicalGet>();
			for (auto &entry : get.table_filters.filters) {
				if (entry.second->filter_type != TableFilterType::EXPRESSION_FILTER) {
					// not an expression filter
					continue;
				}
				auto &expr_filter = entry.second->Cast<ExpressionFilter>();
				if (TryOptimizeGet(binder, context, plan, root, nullptr, entry.first, expr_filter.expr)) {
					return true;
				}
			}
			return false;
		}
		return false;
	}

	static bool TryOptimizeGet(Binder &binder, ClientContext &context, unique_ptr<LogicalOperator> &get_ptr,
	                           unique_ptr<LogicalOperator> &root, optional_ptr<LogicalFilter> filter,
	                           optional_idx filter_column_idx, unique_ptr<Expression> &filter_expr) {
		auto &get = get_ptr->Cast<LogicalGet>();
		if (get.function.name != "seq_scan") {
			return false;
		}

		// We cant optimize if the table already has filters pushed down :(
		if (get.dynamic_filters && get.dynamic_filters->HasFilters()) {
			return false;
		}

		// We can replace the scan function with a rtree index scan (if the table has a rtree index)
		// Get the table
		auto &table = *get.GetTable();
		if (!table.IsDuckTable()) {
			// We can only replace the scan if the table is a duck table
			return false;
		}

		// Find the index
		auto &duck_table = table.Cast<DuckTableEntry>();
		auto &table_info = *table.GetStorage().GetDataTableInfo();
		unique_ptr<RTreeIndexScanBindData> bind_data = nullptr;

		unordered_set<string> spatial_predicates = {"ST_Equals",    "ST_Intersects",      "ST_Touches",  "ST_Crosses",
		                                            "ST_Within",    "ST_Contains",        "ST_Overlaps", "ST_Covers",
		                                            "ST_CoveredBy", "ST_ContainsProperly"};

		table_info.GetIndexes().BindAndScan<RTreeIndex>(context, table_info, [&](RTreeIndex &index_entry) {
			// Create the bind data for this index given the bounding box
			bool rewrite_possible = true;
			auto index_expr = index_entry.unbound_expressions[0]->Copy();
			if (filter_column_idx.IsValid()) {
				RewriteIndexExpressionForFilter(index_entry, get, index_expr, filter_column_idx.GetIndex(),
				                                rewrite_possible);
			} else {
				RewriteIndexExpression(index_entry, get, *index_expr, rewrite_possible);
			}
			if (!rewrite_possible) {
				// Could not rewrite!
				return false;
			}

			FunctionExpressionMatcher matcher;
			matcher.function = make_uniq<ManyFunctionMatcher>(spatial_predicates);
			matcher.expr_type = make_uniq<SpecificExpressionTypeMatcher>(ExpressionType::BOUND_FUNCTION);
			matcher.policy = SetMatcher::Policy::UNORDERED;

			matcher.matchers.push_back(make_uniq<ExpressionEqualityMatcher>(*index_expr));
			matcher.matchers.push_back(make_uniq<ConstantExpressionMatcher>());

			vector<reference<Expression>> bindings;
			if (!matcher.Match(*filter_expr, bindings)) {
				return false;
			}

			// 		bindings[0] = the expression
			// 		bindings[1] = the index expression
			// 		bindings[2] = the constant

			// Compute the bounding box
			auto constant_value = bindings[2].get().Cast<BoundConstantExpression>().value;
			Box2D<float> bbox;
			if (!TryGetBoundingBox(constant_value, bbox)) {
				return false;
			}

			bind_data = make_uniq<RTreeIndexScanBindData>(duck_table, index_entry, bbox);
			return true;
		});

		if (!bind_data) {
			// No index found
			return false;
		}

		// If there are no table filters pushed down into the get, we can just replace the get with the index scan
		get.function = RTreeIndexScanFunction::GetFunction();
		const auto cardinality = get.function.cardinality(context, bind_data.get());
		get.has_estimated_cardinality = cardinality->has_estimated_cardinality;
		get.estimated_cardinality = cardinality->estimated_cardinality;
		get.bind_data = std::move(bind_data);
		if (get.table_filters.filters.empty()) {
			return true;
		}

		// Before we clear projection ids, replace projection map in the filter
		if (!get.projection_ids.empty() && filter) {
			for (auto &id : filter->projection_map) {
				id = get.projection_ids[id];
			}
		}

		get.projection_ids.clear();
		get.types.clear();

		// Otherwise, things get more complicated. We need to pullup the filters from the table scan as our index scan
		// does not support regular filter pushdown.
		auto new_filter = make_uniq<LogicalFilter>();
		auto &column_ids = get.GetColumnIds();
		for (const auto &entry : get.table_filters.filters) {
			idx_t column_id = entry.first;
			auto &type = get.returned_types[column_id];
			bool found = false;
			for (idx_t i = 0; i < column_ids.size(); i++) {
				if (column_ids[i].GetPrimaryIndex() == column_id) {
					column_id = i;
					found = true;
					break;
				}
			}
			if (!found) {
				throw InternalException("Could not find column id for filter");
			}
			auto column = make_uniq<BoundColumnRefExpression>(type, ColumnBinding(get.table_index, column_id));
			new_filter->expressions.push_back(entry.second->ToExpression(*column));
		}
		new_filter->children.push_back(std::move(get_ptr));
		new_filter->ResolveOperatorTypes();
		get_ptr = std::move(new_filter);
		return true;
	}

	static void OptimizeRecursive(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
	                              unique_ptr<LogicalOperator> &root) {
		if (!TryOptimize(input.optimizer.binder, input.context, plan, root)) {
			// No match: continue with the children
			for (auto &child : plan->children) {
				OptimizeRecursive(input, child, root);
			}
		}
	}

	static void Optimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
		OptimizeRecursive(input, plan, plan);
	}
};

//-----------------------------------------------------------------------------
// Register
//-----------------------------------------------------------------------------
void RTreeModule::RegisterIndexPlanScan(DatabaseInstance &db) {
	// Register the optimizer extension
	db.config.optimizer_extensions.push_back(RTreeIndexScanOptimizer());
}

} // namespace duckdb