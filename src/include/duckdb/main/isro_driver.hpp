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
//!     run query with profiling enabled
//!     hash(profile_tree) == prev_hash → break (stable plan)
//!     Γ ← measured join cardinalities (sorted table names → actual rows)
//!
//! Join cardinalities are extracted from the in-memory profiling tree by
//! matching join operators and collecting table names from descendant scans.
class ISRODriver {
public:
	explicit ISRODriver(ClientContext &context);

	//! Run the given query using the ISRO re-optimization loop.
	//! Returns the result of the final (post-stabilization) execution.
	unique_ptr<MaterializedQueryResult> Execute(const string &query);

private:
	ClientContext &context;
};

} // namespace duckdb
