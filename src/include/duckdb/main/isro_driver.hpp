//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/isro_driver.hpp
//
// ISRO: Iterative Sampling-based Re-Optimization driver.
// Provides a high-level API for running a query with ISRO enabled.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {

class ClientContext;
class MaterializedQueryResult;

//! ISRODriver orchestrates the ISRO re-optimization loop for a single query.
//!
//! Algorithm (per Execute()):
//!   Γ = {}
//!   for i = 1 .. max_iterations:
//!     inject Γ into ClientConfig::isro_gamma_overrides
//!     run EXPLAIN ANALYZE <query>  → collect actual per-join cardinalities
//!     hash(plan) == prev_hash → break (stable plan)
//!     Γ  ←  measured cardinalities (sorted table names → actual rows)
//!   run final query (no EXPLAIN) with current Γ injected → return result
//!
//! The per-join cardinality is extracted from the EXPLAIN ANALYZE JSON output
//! by matching operator types HASH_JOIN / NESTED_LOOP_JOIN and reading the
//! "operator_cardinality" field.  The join key is derived from the
//! "estimated_cardinality" fields on the child TABLE_SCAN operators.
class ISRODriver {
public:
	explicit ISRODriver(ClientContext &context);

	//! Run the given query using the ISRO re-optimization loop.
	//! Returns the result of the final (post-stabilization) execution.
	unique_ptr<MaterializedQueryResult> Execute(const string &query);

private:
	ClientContext &context;

	//! Parse the EXPLAIN ANALYZE JSON string and extract a map from
	//! sorted-table-name key → actual row count for each join operator.
	static unordered_map<string, idx_t> ExtractJoinCardinalities(const string &explain_json);

	//! Recursively walk the profiling JSON tree to collect join cardinalities.
	static void WalkProfileTree(const string &json, idx_t pos, vector<string> &accumulated_tables,
	                            unordered_map<string, idx_t> &result);
};

} // namespace duckdb
