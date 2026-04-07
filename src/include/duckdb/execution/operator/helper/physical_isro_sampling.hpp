//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/helper/physical_isro_sampling.hpp
//
// ISRO (Iterative Sampling-based Re-Optimization)
// A passthrough operator that counts rows and signals early termination
// once a configurable sample budget is reached.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include <atomic>

namespace duckdb {

//! PhysicalISROSampling is a passthrough operator inserted after a join to
//! measure its actual output cardinality during a partial execution.
//! When the accumulated row count reaches sample_budget, it writes the
//! measured cardinality to *measured_cardinality and returns FINISHED,
//! which causes the executor to halt the partial execution.
class PhysicalISROSampling : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::ISRO_SAMPLING;

public:
	//! join_key: canonical string identifying the join whose cardinality is being measured
	//! sample_budget: how many rows to pass through before halting
	//! measured_cardinality: shared atomic written when budget is reached
	PhysicalISROSampling(PhysicalPlan &physical_plan, vector<LogicalType> types, string join_key,
	                     idx_t sample_budget, shared_ptr<std::atomic<idx_t>> measured_cardinality);

	//! Key identifying the join relation set (used to populate gamma)
	string join_key;
	//! Maximum rows to accumulate before signalling FINISHED
	idx_t sample_budget;
	//! Shared counter written once budget is exceeded
	shared_ptr<std::atomic<idx_t>> measured_cardinality;

public:
	// Operator interface
	unique_ptr<OperatorState> GetOperatorState(ExecutionContext &context) const override;
	OperatorResultType Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
	                           GlobalOperatorState &gstate, OperatorState &state) const override;

	bool ParallelOperator() const override {
		return false; // single-threaded for deterministic cardinality measurement
	}

	InsertionOrderPreservingMap<string> ParamsToString() const override;
};

} // namespace duckdb
