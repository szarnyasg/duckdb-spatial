#include "spatial/operators/spatial_join_physical.hpp"
#include "spatial/geometry/geometry_type.hpp"
#include "spatial/geometry/sgl.hpp"
#include "spatial/spatial_types.hpp"
#include "spatial_join_logical.hpp"

#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/common/types/row/tuple_data_collection.hpp"
#include "duckdb/common/types/row/tuple_data_iterator.hpp"
#include "duckdb/execution/operator/join/physical_comparison_join.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

//======================================================================================================================
// Flat RTree
//======================================================================================================================

namespace {

template <class T>
class typed_view {
public:
	size_t size() const {
		return len;
	}
	T *data() {
		return ptr;
	}
	const T *data() const {
		return ptr;
	}
	T &operator[](size_t idx) {
		return data()[idx];
	}
	const T &operator[](size_t idx) const {
		return data()[idx];
	}

	void set(T *ptr_p, const size_t len_p) {
		ptr = ptr_p;
		len = len_p;
	}

private:
	T *ptr = nullptr;
	size_t len = 0;
};

class FlatRTreeScanState {
	friend class FlatRTree;
	using Box = Box2D<float>;

public:
	explicit FlatRTreeScanState() : matches(LogicalType::POINTER) {
	}

public:
	Vector matches;
	idx_t matches_count = 0;
	idx_t matches_idx = 0;

private:
	queue<size_t> search_queue;
	Box search_box;
	size_t entry_beg = 0;
	size_t entry_pos = 0;
	bool exhausted = true;
};

class FlatRTree {
public:
	using Box = Box2D<float>;

	FlatRTree(Allocator &alloc, uint32_t item_count_p, uint32_t node_size_p)
	    : item_count(item_count_p), node_size(node_size_p) {

		uint32_t count = item_count;
		uint32_t nodes = item_count;

		layer_bounds.push_back(nodes);

		if (item_count_p == 0) {
			return;
		}

		do {
			count = (count + node_size - 1) / node_size;
			nodes += count;
			layer_bounds.push_back(nodes);
		} while (count > 1);

		box_array_mem = alloc.Allocate(sizeof(Box) * nodes);
		idx_array_mem = alloc.Allocate(sizeof(uint32_t) * nodes);
		row_array_mem = alloc.Allocate(sizeof(data_ptr_t) * item_count);

		box_array.set(reinterpret_cast<Box *>(box_array_mem.get()), nodes);
		idx_array.set(reinterpret_cast<uint32_t *>(idx_array_mem.get()), nodes);
		row_array.set(reinterpret_cast<data_ptr_t *>(row_array_mem.get()), item_count);

		// Make sure that memory is initialized
		for (size_t i = 0; i < nodes; i++) {
			box_array[i] = Box();
			idx_array[i] = 0;
		}
		for (size_t i = 0; i < item_count; i++) {
			row_array[i] = nullptr;
		}
	}

	uint32_t Count() const {
		return item_count;
	}

	// Return insertion index
	uint32_t Push(const Box &box, data_ptr_t row) {
		// Push the index and the box
		idx_array[current_position] = current_position;
		box_array[current_position] = box;

		// Update the bounds
		tree_box.Union(box);

		// Store the row pointer
		row_array[current_position] = row;

		return current_position++;
	}

	void Sort(vector<uint32_t> &curve) {
		Sort(curve, 0, curve.size() - 1);
	}

	void Sort(vector<uint32_t> &curve, size_t l_idx, size_t r_idx) {
		if (l_idx < r_idx) {
			const auto pivot = curve[(l_idx + r_idx) >> 1];
			auto pivot_l = l_idx - 1;
			auto pivot_r = r_idx + 1;

			while (true) {
				do {
					++pivot_l;
				} while (curve[pivot_l] < pivot);
				do {
					--pivot_r;
				} while (curve[pivot_r] > pivot);

				if (pivot_l >= pivot_r) {
					break;
				}

				// Reorder the curve, boxes and indices
				// TODO: Pass callback here and make static
				std::swap(curve[pivot_l], curve[pivot_r]);
				std::swap(box_array[pivot_l], box_array[pivot_r]);
				std::swap(idx_array[pivot_l], idx_array[pivot_r]);
			}

			Sort(curve, l_idx, pivot_r);
			Sort(curve, pivot_r + 1, r_idx);
		}
	}

	void Build() {
		D_ASSERT(item_count == current_position);

		if (item_count <= node_size) {
			box_array[current_position++] = tree_box;
			return;
		}

		// Generate hilbert curve values
		// TODO: Parallelize this with tasks when the number of items is large?
		constexpr auto max_hilbert = std::numeric_limits<uint16_t>::max();
		const auto hw = max_hilbert / (tree_box.max.x - tree_box.min.x);
		const auto hh = max_hilbert / (tree_box.max.y - tree_box.min.y);

		vector<uint32_t> curve(item_count);
		for (idx_t i = 0; i < item_count; i++) {
			const auto &node_box = box_array[i];

			const auto hx = static_cast<uint32_t>(hw * ((node_box.min.x + node_box.max.x) / 2 - tree_box.min.x));
			const auto hy = static_cast<uint32_t>(hh * ((node_box.min.y + node_box.max.y) / 2 - tree_box.min.y));

			curve[i] = sgl::util::hilbert_encode(16, hx, hy);
		}

		// Now, sort the indices based on their curve value
		Sort(curve);

		size_t layer_idx = 0;
		size_t entry_idx = 0;

		while (layer_idx < layer_bounds.size() - 1) {
			const auto entry_end = layer_bounds[layer_idx];

			while (entry_idx < entry_end) {
				auto node_idx = entry_idx;
				auto node_box = box_array[entry_idx];

				size_t child_idx = 0;
				while (child_idx < node_size && entry_idx < entry_end) {

					node_box.Union(box_array[entry_idx]);

					child_idx++;
					entry_idx++;
				}

				// Add a new parent node
				idx_array[current_position] = node_idx;
				box_array[current_position] = node_box;
				current_position++;
			}

			// Go to the next layer
			layer_idx++;
		}
	}

	size_t UpperBound(size_t node_idx) const {
		const auto it = std::upper_bound(layer_bounds.begin(), layer_bounds.end(), node_idx);
		if (it == layer_bounds.end()) {
			return layer_bounds.back();
		}
		return *it;
	}

	void InitScan(FlatRTreeScanState &state, const Box &box) const {
		while (!state.search_queue.empty()) {
			state.search_queue.pop();
		}
		state.search_box = box;
		state.entry_beg = box_array.size() - 1;
		state.entry_pos = state.entry_beg;

		state.exhausted = false;
		state.matches_idx = 0;
		state.matches_count = 0;
	}

	bool Scan(FlatRTreeScanState &state) const {
		if (state.exhausted) {
			return false;
		}

		idx_t count = 0;
		const auto ptr = FlatVector::GetData<data_ptr_t>(state.matches);
		Lookup(state, [&](const data_ptr_t &row) {
			ptr[count++] = row;
			return count == STANDARD_VECTOR_SIZE;
		});
		// Set the count of the result vector
		state.matches_count = count;
		state.matches_idx = 0;

		return count > 0;
	}

	template <class CALLBACK>
	void Lookup(FlatRTreeScanState &state, CALLBACK &&callback) const {

		while (true) {

			const auto entry_end = std::min(state.entry_beg + node_size, UpperBound(state.entry_beg));

			while (state.entry_pos < entry_end) {
				if (!state.search_box.Intersects(box_array[state.entry_pos])) {
					state.entry_pos++;
					continue;
				}

				auto yield = false;

				if (state.entry_beg >= item_count) {
					// Internal node
					state.search_queue.push(idx_array[state.entry_pos]);
				} else {
					// Leaf node
					yield = callback(row_array[idx_array[state.entry_pos]]);
				}

				state.entry_pos++;

				if (yield) {
					// Yield!, return true to signal that there might be more rows!
					return;
				}
			}

			if (state.search_queue.empty()) {
				// There is no more nodes to search, return false!
				state.exhausted = true;
				return;
			}

			state.entry_beg = state.search_queue.front();
			state.entry_pos = state.entry_beg;
			state.search_queue.pop();
		}
	}

private:
	vector<uint32_t> layer_bounds;

	AllocatedData box_array_mem;
	AllocatedData idx_array_mem;
	AllocatedData row_array_mem;

	typed_view<uint32_t> idx_array;
	typed_view<Box> box_array;
	typed_view<data_ptr_t> row_array;

	Box tree_box;

	uint32_t item_count = 0;
	uint32_t node_size = 0;
	uint32_t current_position = 0;
};

} // namespace

//======================================================================================================================
// Physical Spatial Join Operator
//======================================================================================================================

PhysicalSpatialJoin::PhysicalSpatialJoin(LogicalOperator &op, PhysicalOperator &left, PhysicalOperator &right,
                                         unique_ptr<Expression> condition_p, JoinType join_type,
                                         idx_t estimated_cardinality)
    : PhysicalJoin(op, PhysicalOperatorType::EXTENSION, join_type, estimated_cardinality),
      condition(std::move(condition_p)) {

	children.emplace_back(left);
	children.emplace_back(right);

	auto &func = condition->Cast<BoundFunctionExpression>();

	// Extract the probe side and build side join keys
	probe_side_key = func.children[0].get();
	build_side_key = func.children[1].get();

	// Only simple join types are supported
	D_ASSERT(join_type == JoinType::INNER || join_type == JoinType::LEFT || join_type == JoinType::OUTER ||
	         join_type == JoinType::RIGHT);

	// Always make sure we have a consistent order of the output columns, regardless if we have projection maps or not

	const auto &lop = op.Cast<LogicalJoin>();

	// Probe-side
	const auto &probe_side_input_types = children[0].get().types;
	probe_side_output_columns = lop.left_projection_map;
	if (probe_side_output_columns.empty()) {
		probe_side_output_columns.reserve(probe_side_input_types.size());
		for (idx_t i = 0; i < probe_side_input_types.size(); i++) {
			probe_side_output_columns.emplace_back(i);
		}
	}

	for (const auto &probe_col_idx : probe_side_output_columns) {
		const auto type = probe_side_input_types[probe_col_idx];
		probe_side_output_types.push_back(type);
	}

	// The build side is slightly different.
	// Here we reorder the layout so that join keys are first, and the payload columns.
	// For right outer, the match column is added at the end

	unordered_map<idx_t, idx_t> conditions_in_layout;
	// TODO: Loop over multiple conds
	if (build_side_key->GetExpressionClass() == ExpressionClass::BOUND_REF) {
		conditions_in_layout.emplace(build_side_key->Cast<BoundReferenceExpression>().index, 0); // TODO: i, not 0
	}
	// TODO Add rest too
	build_side_key_types.push_back(build_side_key->return_type);

	const auto &build_side_input_types = children[1].get().types;
	auto right_projection_map_copy = lop.right_projection_map;
	if (right_projection_map_copy.empty()) {
		right_projection_map_copy.reserve(build_side_input_types.size());
		for (idx_t i = 0; i < build_side_input_types.size(); i++) {
			right_projection_map_copy.emplace_back(i);
		}
	}

	for (auto &rhs_col : right_projection_map_copy) {
		auto &rhs_type = build_side_input_types[rhs_col];

		auto it = conditions_in_layout.find(rhs_col);
		if (it == conditions_in_layout.end()) {
			// This column is not a condition, but we want to include it in the output.
			// And thus need to add it to the layout
			build_side_output_columns.push_back(build_side_key_types.size() + build_side_payload_types.size());
			build_side_payload_types.push_back(rhs_type);
			build_side_payload_columns.push_back(rhs_col);
		} else {
			// This condition is part of the layout already, so just project it out
			// (conditions are added to the layout separately below)
			build_side_output_columns.push_back(it->second);
		}
		build_side_output_types.push_back(rhs_type);
	}

	vector<LogicalType> layout_types;
	// Insert all condition types
	layout_types.insert(layout_types.end(), build_side_key_types.begin(), build_side_key_types.end());
	layout_types.insert(layout_types.end(), build_side_payload_types.begin(), build_side_payload_types.end());
	if (PropagatesBuildSide(join_type)) {
		// Add it to the ned
		layout_types.push_back(LogicalType::BOOLEAN);
	}

	// Initialize the layout
	// TODO: Align?
	layout = make_shared_ptr<TupleDataLayout>();
	layout->Initialize(std::move(layout_types), false);

	// For right/outer joins, this is where the build side match column goes
	if (PropagatesBuildSide(join_type)) {
		const auto &offsets = layout->GetOffsets();
		build_side_match_offset = offsets[build_side_key_types.size() + build_side_payload_types.size()];
	}
}

InsertionOrderPreservingMap<string> PhysicalSpatialJoin::ParamsToString() const {
	// TODO: Add condition to the result (GetName is wrong)
	auto result = PhysicalOperator::ParamsToString();
	result["Join Type"] = EnumUtil::ToString(join_type);
	result["Conditions"] = condition->GetName();
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

string PhysicalSpatialJoin::GetName() const {
	return "SPATIAL_JOIN";
}

//----------------------------------------------------------------------------------------------------------------------
// Sink Interface
//----------------------------------------------------------------------------------------------------------------------
class SpatialJoinGlobalState final : public GlobalSinkState {
public:
	unique_ptr<TupleDataCollection> collection;

	// The number of non-null and non-empty geometries on the build side
	idx_t total_rtree_size = 0;

	// This is initialized in the finalize state
	unique_ptr<FlatRTree> rtree = nullptr;
};

unique_ptr<GlobalSinkState> PhysicalSpatialJoin::GetGlobalSinkState(ClientContext &context) const {

	auto gstate = make_uniq<SpatialJoinGlobalState>();
	gstate->collection = make_uniq<TupleDataCollection>(BufferManager::GetBufferManager(context), layout);

	return std::move(gstate);
}

class SpatialJoinLocalState final : public LocalSinkState {
public:
	SpatialJoinLocalState(const PhysicalSpatialJoin &op, ClientContext &context,
	                      const shared_ptr<TupleDataLayout> &layout)
	    : build_side_key_executor(context), build_side_filter_executor(context) {
		// Dont keep the tuples in memory after appending.
		collection = make_uniq<TupleDataCollection>(BufferManager::GetBufferManager(context), layout);
		collection->InitializeAppend(append_state, TupleDataPinProperties::UNPIN_AFTER_DONE);

		// TODO: Add other join condition expressions here
		build_side_key_executor.AddExpression(*op.build_side_key);
		build_side_key_chunk.Initialize(context, op.build_side_key_types);

		build_side_row_chunk.InitializeEmpty(layout->GetTypes());

		build_side_payload_chunk.InitializeEmpty(op.build_side_payload_types);

		auto &catalog = Catalog::GetSystemCatalog(context);
		auto &entry = catalog.GetEntry<ScalarFunctionCatalogEntry>(context, DEFAULT_SCHEMA, "ST_IsEmpty");
		auto func = entry.functions.GetFunctionByArguments(context, {GeoTypes::GEOMETRY()});

		auto is_empty_expr = make_uniq<BoundFunctionExpression>(LogicalTypeId::BOOLEAN, func,
		                                                        vector<unique_ptr<Expression>> {}, nullptr);
		is_empty_expr->children.push_back(
		    make_uniq_base<Expression, BoundReferenceExpression>(GeoTypes::GEOMETRY(), 0));

		auto is_not_empty_expr =
		    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_NOT, LogicalTypeId::BOOLEAN);
		is_not_empty_expr->children.push_back(std::move(is_empty_expr));

		auto is_not_null_expr =
		    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalTypeId::BOOLEAN);
		is_not_null_expr->children.push_back(
		    make_uniq_base<Expression, BoundReferenceExpression>(GeoTypes::GEOMETRY(), 0));

		auto filter_expr = make_uniq_base<Expression, BoundConjunctionExpression>(
		    ExpressionType::CONJUNCTION_AND, std::move(is_not_empty_expr), std::move(is_not_null_expr));

		build_side_filter_expr = std::move(filter_expr);
		build_side_filter_executor.AddExpression(*build_side_filter_expr);
		build_side_filter_sel.Initialize(STANDARD_VECTOR_SIZE);
	}

	TupleDataAppendState append_state;
	unique_ptr<TupleDataCollection> collection;

	// Owning DataChunk containing the build side join-key
	DataChunk build_side_key_chunk;
	DataChunk build_side_payload_chunk;
	// Referencing DataChunk chunk, having the same layout as the collection
	DataChunk build_side_row_chunk;
	// Used to execute the build side join key expression
	ExpressionExecutor build_side_key_executor;

	// Used to count how many non-null and non-empty geometries we have on the build side
	// (so we can initialize the rtree to the correct size)
	unique_ptr<Expression> build_side_filter_expr;
	ExpressionExecutor build_side_filter_executor;
	SelectionVector build_side_filter_sel; // not used, but needed for the executor

	idx_t build_side_non_null_non_empty_count = 0;
};

unique_ptr<LocalSinkState> PhysicalSpatialJoin::GetLocalSinkState(ExecutionContext &context) const {
	// auto &gstate = sink_state->Cast<SpatialJoinGlobalState>();
	auto lstate = make_uniq<SpatialJoinLocalState>(*this, context.client, layout);
	return std::move(lstate);
}

SinkResultType PhysicalSpatialJoin::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {

	auto &lstate = input.local_state.Cast<SpatialJoinLocalState>();

	lstate.build_side_key_chunk.Reset();
	lstate.build_side_key_executor.Execute(chunk, lstate.build_side_key_chunk);

	// Count how many non-null and non-empty geometries we have on the build side
	lstate.build_side_non_null_non_empty_count +=
	    lstate.build_side_filter_executor.SelectExpression(lstate.build_side_key_chunk, lstate.build_side_filter_sel);

	if (build_side_payload_types.empty()) {
		// There are only keys. Make the payload chunk empty
		lstate.build_side_payload_chunk.SetCardinality(chunk.size());
	} else {
		lstate.build_side_payload_chunk.ReferenceColumns(chunk, build_side_payload_columns);
	}

	// Now reference the key chunk, the payload chunk, and the build side match column (if needed)
	idx_t layout_col_idx = 0;
	for (auto &key_col : lstate.build_side_key_chunk.data) {
		lstate.build_side_row_chunk.data[layout_col_idx++].Reference(key_col);
	}

	for (auto &payload_col : lstate.build_side_payload_chunk.data) {
		lstate.build_side_row_chunk.data[layout_col_idx++].Reference(payload_col);
	}

	if (PropagatesBuildSide(join_type)) {
		lstate.build_side_row_chunk.data[layout_col_idx++].Reference(Value::BOOLEAN(false));
	}

	// Set the cardinality to match the input
	lstate.build_side_row_chunk.SetCardinality(chunk.size());

	// Sink the build side chunk
	lstate.collection->Append(lstate.append_state, lstate.build_side_row_chunk);

	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalSpatialJoin::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &gstate = input.global_state.Cast<SpatialJoinGlobalState>();
	auto &lstate = input.local_state.Cast<SpatialJoinLocalState>();

	// Flush the append state
	lstate.collection->FinalizePinState(lstate.append_state.pin_state);

	// Append the local collection to the global collection
	gstate.collection->Combine(*lstate.collection);

	// Merge the non-null and non-empty count
	gstate.total_rtree_size += lstate.build_side_non_null_non_empty_count;

	return SinkCombineResultType::FINISHED;
}

// This is where we would build the rtree, by iterating through the tupledata collection we've created
SinkFinalizeType PhysicalSpatialJoin::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                               OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<SpatialJoinGlobalState>();

	if (gstate.collection->Count() == 0) {
		return EmptyResultIfRHSIsEmpty() ? SinkFinalizeType::NO_OUTPUT_POSSIBLE : SinkFinalizeType::READY;
	}

	// Initialize the flat R-Tree
	static constexpr auto RTREE_NODE_SIZE = 32;
	gstate.rtree = make_uniq<FlatRTree>(BufferAllocator::Get(context), gstate.total_rtree_size, RTREE_NODE_SIZE);

	// Now, this is where we build the rtree, by iterating over the tuples in the collection.
	// We need to keep everything pinned so that we can probe the pointers later
	TupleDataChunkIterator iterator(*gstate.collection, TupleDataPinProperties::KEEP_EVERYTHING_PINNED, true);

	const auto rows_ptr = iterator.GetRowLocations();
	Vector row_pointer_vector(LogicalType::POINTER, reinterpret_cast<data_ptr_t>(rows_ptr));

	auto &sel = *FlatVector::IncrementalSelectionVector();
	Vector geom_vec(GeoTypes::GEOMETRY());

	do {
		const auto row_count = iterator.GetCurrentChunkCount();

		// We only need to fetch the build-side key column to build the rtree.
		// The key column is always the first column in the layout.
		constexpr auto build_side_key_col = 0;      // TODO: layout_key_col_idx
		D_ASSERT(build_side_key_types.size() == 1); // TODO: remove this

		gstate.collection->Gather(row_pointer_vector, sel, row_count, build_side_key_col, geom_vec, sel, nullptr);

		// Get a pointer to what we just gathered
		const auto geom_ptr = FlatVector::GetData<geometry_t>(geom_vec);
		// Push the bounding boxes into the R-Tree
		for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
			if (FlatVector::IsNull(geom_vec, row_idx)) {
				// Skip null geometries
				continue;
			}

			const auto &geom = geom_ptr[row_idx];
			Box2D<float> bbox;
			if (!geom.TryGetCachedBounds(bbox)) {
				// Skip empty geometries
				continue;
			}

			// Push the bounding box into the R-Tree
			gstate.rtree->Push(bbox, rows_ptr[row_idx]);
		}
	} while (iterator.Next());

	// Build the R-Tree once we've gathered everything
	gstate.rtree->Build();

	return SinkFinalizeType::READY;
}

//----------------------------------------------------------------------------------------------------------------------
// Operator Interface
//----------------------------------------------------------------------------------------------------------------------
// This is where we do the probing of the rtree.

enum class SpatialJoinState { START = 0, INIT, PROBE, SCAN, EMIT, EMIT_LHS };

class SpatialJoinLocalOperatorState final : public CachingOperatorState {
public:
	bool is_initialized = false;

	idx_t input_index = 0;
	SpatialJoinState state = SpatialJoinState::START;

	DataChunk overflow_matches;

	FlatRTreeScanState scan;

	DataChunk probe_side_row_chunk; // holds the projected lhs columns
	DataChunk probe_side_key_chunk; // holds the lhs probe key
	DataChunk build_side_key_chunk; // holds the rhs build key
	DataChunk match_pred_arg_chunk; // references the lhs probe key and the rhs build key, used to compute the predicate

	ExpressionExecutor join_probe_executor; // used to compute the probe key
	ExpressionExecutor join_match_executor; // used to compute the predicate

	UnifiedVectorFormat probe_side_key_vformat; // used to access the probe side key, after its been computed

	unique_ptr<Expression> match_expr;

	SelectionVector probe_side_source_sel; // maps what output rows correspond to which input lhs rows
	SelectionVector build_side_source_sel; // used when gathering the build side
	SelectionVector build_side_target_sel; // used when gathering the build side
	SelectionVector match_sel;             // used to select the output rows, by executing the predicate

	SelectionVector lhs_match_sel; // this is only used when emitting the lhs side of the join

	uint8_t left_outer_marker[STANDARD_VECTOR_SIZE] = {};

	idx_t build_side_match_offset = 0;
	unsafe_unique_array<data_ptr_t> build_side_pointers = nullptr;

	explicit SpatialJoinLocalOperatorState(ClientContext &context)
	    : join_probe_executor(context), join_match_executor(context), probe_side_source_sel(STANDARD_VECTOR_SIZE),
	      build_side_source_sel(STANDARD_VECTOR_SIZE), build_side_target_sel(STANDARD_VECTOR_SIZE),
	      match_sel(STANDARD_VECTOR_SIZE), lhs_match_sel(STANDARD_VECTOR_SIZE) {

		build_side_pointers = make_unsafe_uniq_array<data_ptr_t>(STANDARD_VECTOR_SIZE);
	}
};

class SpatialJoinGlobalOperatorState final : public GlobalOperatorState {
public:
	unique_ptr<FlatRTree> rtree;
	unique_ptr<TupleDataCollection> collection;
};

unique_ptr<OperatorState> PhysicalSpatialJoin::GetOperatorState(ExecutionContext &context) const {
	auto lstate = make_uniq<SpatialJoinLocalOperatorState>(context.client);

	// Create a match expression using the condition, that will be used to filter the results
	lstate->match_expr = condition->Copy();
	auto &func_expr = lstate->match_expr->Cast<BoundFunctionExpression>();
	func_expr.children[0] = make_uniq<BoundReferenceExpression>(probe_side_key->return_type, 0);
	func_expr.children[1] = make_uniq<BoundReferenceExpression>(build_side_key->return_type, 1);

	lstate->join_match_executor.AddExpression(*lstate->match_expr);

	// Add the probe side join key expression
	lstate->join_probe_executor.AddExpression(*probe_side_key);

	// The chunks we need for the join
	lstate->probe_side_row_chunk.Initialize(context.client, probe_side_output_types);
	lstate->probe_side_key_chunk.Initialize(context.client, {probe_side_key->return_type});
	lstate->build_side_key_chunk.Initialize(context.client, {build_side_key->return_type});
	lstate->match_pred_arg_chunk.Initialize(context.client, {probe_side_key->return_type, build_side_key->return_type});

	return std::move(lstate);
}

unique_ptr<GlobalOperatorState> PhysicalSpatialJoin::GetGlobalOperatorState(ClientContext &context) const {
	auto &gstate = sink_state->Cast<SpatialJoinGlobalState>();

	auto result = make_uniq<SpatialJoinGlobalOperatorState>();
	// Steal the built rtree from the sink state.
	result->rtree = std::move(gstate.rtree);
	// Steal the tuple data collection
	result->collection = std::move(gstate.collection);

	return std::move(result);
}

OperatorResultType PhysicalSpatialJoin::ExecuteInternal(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                        GlobalOperatorState &gstate_p, OperatorState &lstate_p) const {
	auto &gstate = gstate_p.Cast<SpatialJoinGlobalOperatorState>();
	auto &lstate = lstate_p.Cast<SpatialJoinLocalOperatorState>();

	idx_t output_index = 0;
	idx_t output_count = chunk.GetCapacity();

	while (true) {
		switch (lstate.state) {
		//--------------------------------------------------------------------------------------------------------------
		// START
		//--------------------------------------------------------------------------------------------------------------
		case SpatialJoinState::START: {
			// Check if the build side is empty
			if (gstate.rtree == nullptr || gstate.rtree->Count() == 0) {
				if (EmptyResultIfRHSIsEmpty()) {
					return OperatorResultType::FINISHED;
				}

				// Slice the input chunk to what we need
				lstate.probe_side_row_chunk.ReferenceColumns(input, probe_side_output_columns);

				// Construct empty join result
				PhysicalComparisonJoin::ConstructEmptyJoinResult(join_type, false, lstate.probe_side_row_chunk, chunk);

				return OperatorResultType::NEED_MORE_INPUT;
			}

			lstate.state = SpatialJoinState::INIT;
		} // fall through
		//--------------------------------------------------------------------------------------------------------------
		// INIT
		//--------------------------------------------------------------------------------------------------------------
		case SpatialJoinState::INIT: {
			// We have a new fresh input chunk
			lstate.join_probe_executor.Execute(input, lstate.probe_side_key_chunk);
			lstate.probe_side_key_chunk.data[0].ToUnifiedFormat(input.size(), lstate.probe_side_key_vformat);

			// Reference the columns that we actually care about
			lstate.probe_side_row_chunk.ReferenceColumns(input, probe_side_output_columns);

			// zero miss vector
			memset(lstate.left_outer_marker, 0, sizeof(lstate.left_outer_marker));

			// Reset the input index and move on to the next state
			lstate.input_index = 0;
			lstate.state = SpatialJoinState::PROBE;

		} // fall through
		//--------------------------------------------------------------------------------------------------------------
		// PROBE
		//--------------------------------------------------------------------------------------------------------------
		case SpatialJoinState::PROBE: {
			if (lstate.input_index == input.size()) {
				lstate.state = SpatialJoinState::EMIT;
				continue;
			}

			const auto geom_idx = lstate.probe_side_key_vformat.sel->get_index(lstate.input_index);
			if (!lstate.probe_side_key_vformat.validity.RowIsValid(geom_idx)) {
				lstate.input_index++;
				continue;
			}

			const auto geom_ptr = UnifiedVectorFormat::GetData<geometry_t>(lstate.probe_side_key_vformat);
			const auto &geom = geom_ptr[geom_idx];

			Box2D<float> bbox;
			if (!geom.TryGetCachedBounds(bbox)) {
				lstate.input_index++;
				continue;
			}

			gstate.rtree->InitScan(lstate.scan, bbox);

			if (!gstate.rtree->Scan(lstate.scan)) {
				lstate.input_index++;
				continue;
			}

			lstate.state = SpatialJoinState::SCAN;
		} // fall through
		//--------------------------------------------------------------------------------------------------------------
		// SCAN
		//--------------------------------------------------------------------------------------------------------------
		case SpatialJoinState::SCAN: {
			const auto matches_remaining = lstate.scan.matches_count - lstate.scan.matches_idx;
			if (matches_remaining == 0) {
				// We are out of matches. Try to get the next probe
				if (gstate.rtree->Scan(lstate.scan)) {
					continue;
				}
				// Otherwise, we are done with this probe
				lstate.input_index++;
				lstate.state = SpatialJoinState::PROBE;
				continue;
			}

			const auto output_remaining = output_count - output_index;
			const auto scan_count = MinValue(output_remaining, matches_remaining);

			D_ASSERT(scan_count != 0);

			for (idx_t i = 0; i < scan_count; i++) {
				// These control what we gather from the build side
				lstate.build_side_target_sel.set_index(i, output_index + i);
				lstate.build_side_source_sel.set_index(i, lstate.scan.matches_idx + i);

				// This stores which probe side row we used to gather the build side
				lstate.probe_side_source_sel.set_index(output_index + i, lstate.input_index);
			}

			// Fetch each column from the build side
			D_ASSERT(build_side_key_types.size() == 1);
			// TODO: Add more key conditions here
			constexpr auto build_side_key_col = 0;

			auto &row_pointers = lstate.scan.matches;

			// Collect the build side join key(s)
			// TODO: Multiple join keys
			gstate.collection->Gather(row_pointers, lstate.build_side_source_sel, scan_count, build_side_key_col,
			                          lstate.build_side_key_chunk.data[0], lstate.build_side_target_sel, nullptr);

			// Now, lets collect the rest of the build side columns
			for (idx_t i = 0; i < build_side_output_columns.size(); i++) {
				auto &target = chunk.data[probe_side_output_columns.size() + i];
				const auto build_side_col_idx = build_side_output_columns[i];
				D_ASSERT(target.GetType() == build_side_output_types[i]);

				// TODO: We should use cached cast vectors here to improve performance of nested array payloads
				gstate.collection->Gather(row_pointers, lstate.build_side_source_sel, scan_count, build_side_col_idx,
				                          target, lstate.build_side_target_sel, nullptr);
			}

			// Also collect the build side row pointers (if we have a match column)
			if (IsRightOuterJoin(join_type)) {
				const auto ptrs = FlatVector::GetData<data_ptr_t>(row_pointers);
				for (idx_t i = 0; i < scan_count; i++) {
					lstate.build_side_pointers[output_index + i] = ptrs[i];
				}
			}

			// Increment the output and match index
			output_index += scan_count;
			lstate.scan.matches_idx += scan_count;

			if (output_index != output_count) {
				// We still have space left. Scan more!
				continue;
			}

			lstate.state = SpatialJoinState::EMIT;
		} // fall through
		//--------------------------------------------------------------------------------------------------------------
		// EMIT
		//--------------------------------------------------------------------------------------------------------------
		case SpatialJoinState::EMIT: {

			// Start by adding the lhs columns
			chunk.Slice(lstate.probe_side_row_chunk, lstate.probe_side_source_sel, output_index);

			// Now, lets actually evaluate the predicate
			lstate.match_pred_arg_chunk.data[0].Slice(lstate.probe_side_key_chunk.data[0], lstate.probe_side_source_sel,
			                                          output_index);
			lstate.match_pred_arg_chunk.data[1].Reference(lstate.build_side_key_chunk.data[0]);
			lstate.match_pred_arg_chunk.SetCardinality(output_index);

			const auto filtered =
			    lstate.join_match_executor.SelectExpression(lstate.match_pred_arg_chunk, lstate.match_sel);

			if (IsLeftOuterJoin(join_type)) {
				for (idx_t i = 0; i < filtered; i++) {
					// This is kinda crazy
					// We're first selecting all the output rows that matched.
					// And then figuring out which lhs row that belongs to, by using the probe_side_source_sel vector
					const auto match_source_idx = lstate.match_sel.get_index(i);
					const auto probe_source_idx = lstate.probe_side_source_sel.get_index(match_source_idx);
					lstate.left_outer_marker[probe_source_idx] = 1;
				}
			}

			if (IsRightOuterJoin(join_type)) {
				// Mark each row in the build side as being matched.
				// We need to do this so we dont emit them again in the right-outer join phase

				// This is where the bool match offset is
				const auto tuple_size = build_side_match_offset;

				for (idx_t i = 0; i < filtered; i++) {
					const auto match_source_idx = lstate.match_sel.get_index(i);
					const auto data_ptr = lstate.build_side_pointers[match_source_idx] + tuple_size;
					// Set the match column to true
					Store<bool>(true, data_ptr);
				}
			}

			chunk.Slice(lstate.match_sel, filtered);

			if (lstate.input_index != input.size()) {
				// We still have more input rows to process
				lstate.state = SpatialJoinState::SCAN;
				return OperatorResultType::HAVE_MORE_OUTPUT;
			}

			if (IsLeftOuterJoin(join_type)) {
				// Before we can ask for more input, we need to emit the outer left side
				// But we need a clean output chunk, so we need to return here (cant just fall through)

				lstate.state = SpatialJoinState::EMIT_LHS;
				return OperatorResultType::HAVE_MORE_OUTPUT;
			}

			// Otherwise, we need to wait for more input
			lstate.state = SpatialJoinState::INIT;
			return OperatorResultType::NEED_MORE_INPUT;
		}
		//--------------------------------------------------------------------------------------------------------------
		// EMIT LEFT OUTER
		//--------------------------------------------------------------------------------------------------------------
		case SpatialJoinState::EMIT_LHS: {
			// Emit the outer left side

			idx_t remaining_count = 0;
			for (idx_t i = 0; i < input.size(); i++) {
				if (!lstate.left_outer_marker[i]) {
					lstate.lhs_match_sel.set_index(remaining_count++, i);
				}
			}

			if (remaining_count > 0) {
				chunk.Slice(lstate.probe_side_row_chunk, lstate.lhs_match_sel, remaining_count);

				// Null the RHS columns
				for (idx_t i = 0; i < build_side_output_columns.size(); i++) {
					auto &target = chunk.data[probe_side_output_columns.size() + i];
					target.SetVectorType(VectorType::CONSTANT_VECTOR);
					ConstantVector::SetNull(target, true);
				}
			}

			lstate.state = SpatialJoinState::INIT;
			return OperatorResultType::NEED_MORE_INPUT;
		}
		default:
			D_ASSERT(false);
			break;
		}
	}
}

//----------------------------------------------------------------------------------------------------------------------
// Source Interface
//----------------------------------------------------------------------------------------------------------------------
class SpatialJoinGlobalSourceState final : public GlobalSourceState {
public:
	explicit SpatialJoinGlobalSourceState(const PhysicalSpatialJoin &op) : op(op) {
		D_ASSERT(op.sink_state);
		const auto &state = op.op_state->Cast<SpatialJoinGlobalOperatorState>();

		// Initialize a parallel scan
		vector<column_t> column_ids;

		column_ids.insert(column_ids.end(), op.build_side_output_columns.begin(), op.build_side_output_columns.end());

		// Also add the match column
		column_ids.push_back(op.build_side_key_types.size() + op.build_side_payload_types.size());

		// We dont need to keep the tuples aroun after scanning
		state.collection->InitializeScan(scan_state, std::move(column_ids), TupleDataPinProperties::DESTROY_AFTER_DONE);

		tuples_maximum = state.collection->Count();
	}

	const PhysicalSpatialJoin &op;
	TupleDataParallelScanState scan_state;

	// How many tuples we have scanned so far
	idx_t tuples_maximum = 0;
	atomic<idx_t> tuples_scanned = {0};

public:
	idx_t MaxThreads() override {
		const auto &state = op.op_state->Cast<SpatialJoinGlobalOperatorState>();
		const auto count = state.collection->Count();

		// Rough approximation of the number of threads to use
		return count / (STANDARD_VECTOR_SIZE * 10ULL);
	}
};

class SpatialJoinLocalSourceState final : public LocalSourceState {
public:
	explicit SpatialJoinLocalSourceState(const PhysicalSpatialJoin &op) : match_sel(STANDARD_VECTOR_SIZE) {

		D_ASSERT(op.sink_state);
		const auto &state = op.op_state->Cast<SpatialJoinGlobalOperatorState>();

		vector<column_t> column_ids;

		column_ids.insert(column_ids.end(), op.build_side_output_columns.begin(), op.build_side_output_columns.end());

		// Also add the match column
		column_ids.push_back(op.build_side_key_types.size() + op.build_side_payload_types.size());

		// We dont need to keep the tuples aroun after scanning
		state.collection->InitializeScan(scan_state, std::move(column_ids), TupleDataPinProperties::DESTROY_AFTER_DONE);
		state.collection->InitializeScanChunk(scan_state, scan_chunk);
	}

	TupleDataLocalScanState scan_state;
	DataChunk scan_chunk;
	SelectionVector match_sel;
};

unique_ptr<GlobalSourceState> PhysicalSpatialJoin::GetGlobalSourceState(ClientContext &context) const {
	auto gstate = make_uniq<SpatialJoinGlobalSourceState>(*this);
	return std::move(gstate);
}

unique_ptr<LocalSourceState> PhysicalSpatialJoin::GetLocalSourceState(ExecutionContext &context,
                                                                      GlobalSourceState &gstate_p) const {
	auto lstate = make_uniq<SpatialJoinLocalSourceState>(*this);
	return std::move(lstate);
}

SourceResultType PhysicalSpatialJoin::GetData(ExecutionContext &context, DataChunk &chunk,
                                              OperatorSourceInput &input) const {
	D_ASSERT(PropagatesBuildSide(join_type));

	auto &gstate = input.global_state.Cast<SpatialJoinGlobalSourceState>();
	auto &lstate = input.local_state.Cast<SpatialJoinLocalSourceState>();

	const auto &tuples = gstate.op.op_state->Cast<SpatialJoinGlobalOperatorState>().collection;

	while (tuples->Scan(gstate.scan_state, lstate.scan_state, lstate.scan_chunk)) {
		gstate.tuples_scanned += lstate.scan_chunk.size();

		const auto matches = FlatVector::GetData<bool>(lstate.scan_chunk.data.back());

		idx_t result_count = 0;
		for (idx_t i = 0; i < lstate.scan_chunk.size(); i++) {
			if (!matches[i]) {
				lstate.match_sel.set_index(result_count++, i);
			}
		}

		if (result_count > 0) {

			const auto lhs_col_count = probe_side_output_columns.size();
			const auto rhs_col_count = build_side_output_columns.size();

			// Null the LHS columns
			for (idx_t i = 0; i < lhs_col_count; i++) {
				auto &target = chunk.data[i];
				target.SetVectorType(VectorType::CONSTANT_VECTOR);
				ConstantVector::SetNull(target, true);
			}

			// Set the RHS columns
			for (idx_t i = 0; i < rhs_col_count; i++) {
				auto &target = chunk.data[lhs_col_count + i];
				// Offset by one here to skip the match column
				target.Slice(lstate.scan_chunk.data[i], lstate.match_sel, result_count);
			}

			chunk.SetCardinality(result_count);
			return SourceResultType::HAVE_MORE_OUTPUT;
		}
	}

	return SourceResultType::FINISHED;
}

//----------------------------------------------------------------------------------------------------------------------
// Misc
//----------------------------------------------------------------------------------------------------------------------

ProgressData PhysicalSpatialJoin::GetProgress(ClientContext &context, GlobalSourceState &gstate) const {
	const auto &state = gstate.Cast<SpatialJoinGlobalSourceState>();
	ProgressData res;
	if (state.tuples_maximum) {
		res.done = state.tuples_scanned.load();
		res.total = state.tuples_maximum;
	} else {
		res.SetInvalid();
	}
	return res;
}

} // namespace duckdb