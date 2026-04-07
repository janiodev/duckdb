#include "duckdb/execution/operator/helper/physical_isro_sampling.hpp"
#include "duckdb/execution/physical_operator_states.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Operator State
//===--------------------------------------------------------------------===//

class ISROSamplingState : public OperatorState {
public:
	ISROSamplingState() : rows_seen(0), finished(false) {
	}

	//! Running total of rows seen so far in this partial execution
	idx_t rows_seen;
	//! Whether we have already signalled FINISHED
	bool finished;
};

//===--------------------------------------------------------------------===//
// Constructor
//===--------------------------------------------------------------------===//

PhysicalISROSampling::PhysicalISROSampling(PhysicalPlan &physical_plan, vector<LogicalType> types, string join_key_p,
                                           idx_t sample_budget_p,
                                           shared_ptr<std::atomic<idx_t>> measured_cardinality_p)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::ISRO_SAMPLING, std::move(types), 0),
      join_key(std::move(join_key_p)), sample_budget(sample_budget_p),
      measured_cardinality(std::move(measured_cardinality_p)) {
}

//===--------------------------------------------------------------------===//
// Operator Interface
//===--------------------------------------------------------------------===//

unique_ptr<OperatorState> PhysicalISROSampling::GetOperatorState(ExecutionContext & /*context*/) const {
	return make_uniq<ISROSamplingState>();
}

OperatorResultType PhysicalISROSampling::Execute(ExecutionContext & /*context*/, DataChunk &input, DataChunk &chunk,
                                                 GlobalOperatorState & /*gstate*/, OperatorState &state_p) const {
	auto &state = state_p.Cast<ISROSamplingState>();

	if (state.finished) {
		// Budget already reached in a previous call; keep halting.
		return OperatorResultType::FINISHED;
	}

	// Pass the chunk through unchanged.
	chunk.Reference(input);
	state.rows_seen += input.size();

	if (state.rows_seen >= sample_budget) {
		// Record the measured cardinality and signal the executor to stop.
		measured_cardinality->store(state.rows_seen, std::memory_order_relaxed);
		state.finished = true;
		return OperatorResultType::FINISHED;
	}
	return OperatorResultType::NEED_MORE_INPUT;
}

InsertionOrderPreservingMap<string> PhysicalISROSampling::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["join_key"] = join_key;
	result["sample_budget"] = to_string(sample_budget);
	return result;
}

} // namespace duckdb
