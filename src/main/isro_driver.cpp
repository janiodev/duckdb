#include "duckdb/main/isro_driver.hpp"

#include "duckdb/common/enums/metric_type.hpp"
#include "duckdb/common/enums/physical_operator_type.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/main/profiling_info.hpp"
#include "duckdb/main/profiling_node.hpp"
#include "duckdb/main/query_profiler.hpp"

#include <algorithm>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Constructor
//===--------------------------------------------------------------------===//

ISRODriver::ISRODriver(ClientContext &context_p) : context(context_p) {
}

//===--------------------------------------------------------------------===//
// Internal helpers — profiling tree walking
//===--------------------------------------------------------------------===//

// Recursively collect all TABLE_SCAN table names below a given ProfilingNode.
static void CollectTableNames(ProfilingNode &node, vector<string> &names) {
	auto &info = node.GetProfilingInfo();
	if (info.metrics.count(MetricType::OPERATOR_TYPE)) {
		auto op_type = PhysicalOperatorType(info.metrics.at(MetricType::OPERATOR_TYPE).GetValue<uint8_t>());
		if (op_type == PhysicalOperatorType::TABLE_SCAN) {
			// EXTRA_INFO is a MAP(VARCHAR, VARCHAR) — each entry is STRUCT({key, value}).
			if (info.metrics.count(MetricType::EXTRA_INFO)) {
				auto &map_val = info.metrics.at(MetricType::EXTRA_INFO);
				if (!map_val.IsNull()) {
					auto &pairs = MapValue::GetChildren(map_val);
					for (auto &pair : pairs) {
						auto &kv = StructValue::GetChildren(pair);
						if (kv.size() == 2) {
							string key = kv[0].ToString();
							if (key == "Table") {
								// Qualified name may be "catalog.schema.table" — take last part.
								auto full_name = kv[1].ToString();
								auto parts = StringUtil::Split(full_name, ".");
								names.push_back(StringUtil::Lower(parts.back()));
								break;
							}
						}
					}
				}
			}
		}
	}
	for (idx_t i = 0; i < node.GetChildCount(); i++) {
		CollectTableNames(*node.GetChild(i), names);
	}
}

// Recursively extract per-join cardinalities.  Returns accumulated names for
// each sub-expression so the caller can build the full relation set.
static void ExtractJoinCardinalities(ProfilingNode &node, unordered_map<string, idx_t> &gamma) {
	auto &info = node.GetProfilingInfo();
	bool is_join = false;
	if (info.metrics.count(MetricType::OPERATOR_TYPE)) {
		auto op_type = PhysicalOperatorType(info.metrics.at(MetricType::OPERATOR_TYPE).GetValue<uint8_t>());
		is_join = (op_type == PhysicalOperatorType::HASH_JOIN ||
		           op_type == PhysicalOperatorType::NESTED_LOOP_JOIN ||
		           op_type == PhysicalOperatorType::PIECEWISE_MERGE_JOIN ||
		           op_type == PhysicalOperatorType::IE_JOIN);
	}

	if (is_join && info.metrics.count(MetricType::OPERATOR_CARDINALITY)) {
		idx_t card = info.metrics.at(MetricType::OPERATOR_CARDINALITY).GetValue<idx_t>();
		// Collect all table names in this join's subtree.
		vector<string> names;
		CollectTableNames(node, names);
		if (!names.empty()) {
			std::sort(names.begin(), names.end());
			// Remove duplicates (in case of self-joins etc.)
			names.erase(std::unique(names.begin(), names.end()), names.end());
			auto key = StringUtil::Join(names, ",");
			// Update: use max measured if seen multiple times.
			auto it = gamma.find(key);
			if (it == gamma.end() || card > it->second) {
				gamma[key] = card;
			}
		}
	}

	// Recurse into children regardless.
	for (idx_t i = 0; i < node.GetChildCount(); i++) {
		ExtractJoinCardinalities(*node.GetChild(i), gamma);
	}
}

//===--------------------------------------------------------------------===//
// Main execution loop
//===--------------------------------------------------------------------===//

unique_ptr<MaterializedQueryResult> ISRODriver::Execute(const string &query) {
	auto &cfg = ClientConfig::GetConfig(context);

	// Save original profiler settings so we can restore them.
	bool original_profiler_enabled = cfg.enable_profiler;
	bool original_emit_output = cfg.emit_profiler_output;

	// Enable profiling so we collect actual per-operator cardinalities.
	cfg.enable_profiler = true;
	cfg.emit_profiler_output = false; // don't print; we'll read programmatically

	// Reset any stale gamma overrides from a prior ISRO run.
	cfg.isro_gamma_overrides.clear();

	Connection conn(DatabaseInstance::GetDatabase(context));
	unordered_map<string, idx_t> gamma;
	string prev_plan_hash;

	unique_ptr<MaterializedQueryResult> last_result;

	for (idx_t iter = 0; iter < cfg.isro_max_iterations; iter++) {
		// Set current gamma into ClientConfig before the optimizer runs.
		cfg.isro_gamma_overrides = gamma;

		// Run the query.
		auto result = conn.Query(query);
		if (result->HasError()) {
			// Restore and forward the error.
			cfg.enable_profiler = original_profiler_enabled;
			cfg.emit_profiler_output = original_emit_output;
			cfg.isro_gamma_overrides.clear();
			return result;
		}

		// Compute a simple plan hash from the profiling tree's operator type sequence.
		auto &profiler = QueryProfiler::Get(context);
		string plan_hash = profiler.ToJSON(); // use full JSON as a stable plan identity

		if (iter > 0 && plan_hash == prev_plan_hash) {
			// Plan stabilized — we're done.
			last_result = std::move(result);
			break;
		}
		prev_plan_hash = plan_hash;
		last_result = std::move(result);

		// After the last iteration, don't bother collecting more cardinalities.
		if (iter + 1 == cfg.isro_max_iterations) {
			break;
		}

		// Extract per-join actual cardinalities from the profiling tree.
		unordered_map<string, idx_t> new_gamma;
		profiler.GetRootUnderLock([&](optional_ptr<ProfilingNode> root) {
			if (root) {
				ExtractJoinCardinalities(*root, new_gamma);
			}
		});

		// Merge: prefer larger measured cardinalities (more data = better estimate).
		for (auto &kv : new_gamma) {
			auto it = gamma.find(kv.first);
			if (it == gamma.end() || kv.second > it->second) {
				gamma[kv.first] = kv.second;
			}
		}

		if (gamma.empty()) {
			// No joins found — nothing to override.
			break;
		}
	}

	// Restore original profiling settings.
	cfg.enable_profiler = original_profiler_enabled;
	cfg.emit_profiler_output = original_emit_output;
	cfg.isro_gamma_overrides.clear();

	return last_result;
}

} // namespace duckdb
