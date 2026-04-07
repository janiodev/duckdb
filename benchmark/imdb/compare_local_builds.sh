#!/usr/bin/env bash
set -euo pipefail

# Compare two local DuckDB benchmark_runner binaries on a fixed JOB/IMDB track.
# Output: CSV with per-query medians and summary stats.

usage() {
  cat <<'EOF'
Usage:
  compare_local_builds.sh \
    --baseline /path/to/baseline/benchmark_runner \
    --candidate /path/to/isro/benchmark_runner \
    --output /path/to/results_local_compare.csv

Optional:
  --track "01a,01b,01c,..."   Comma-separated JOB IDs (default: planned 20-query track)

Notes:
  - Each benchmark is executed by benchmark_runner with its default run-count.
  - The script computes median timing per query for each binary.
EOF
}

BASELINE_BIN=""
CANDIDATE_BIN=""
OUTPUT_CSV=""
TRACK_IDS="01a,01b,01c,01d,02a,02b,02c,02d,03a,03b,03c,04a,04b,04c,05a,05b,05c,06a,06b,06c"
CACHE_DB=""
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --baseline)
      BASELINE_BIN="$2"
      shift 2
      ;;
    --candidate)
      CANDIDATE_BIN="$2"
      shift 2
      ;;
    --output)
      OUTPUT_CSV="$2"
      shift 2
      ;;
    --track)
      TRACK_IDS="$2"
      shift 2
      ;;
    --cache-db)
      CACHE_DB="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ -z "$BASELINE_BIN" || -z "$CANDIDATE_BIN" || -z "$OUTPUT_CSV" ]]; then
  usage
  exit 1
fi

if [[ ! -x "$BASELINE_BIN" ]]; then
  echo "Baseline binary is not executable: $BASELINE_BIN" >&2
  exit 1
fi
if [[ ! -x "$CANDIDATE_BIN" ]]; then
  echo "Candidate binary is not executable: $CANDIDATE_BIN" >&2
  exit 1
fi

if [[ -z "$CACHE_DB" ]]; then
  echo "Missing required --cache-db argument for local IMDB cache." >&2
  exit 1
fi

if [[ ! -f "$CACHE_DB" ]]; then
  echo "Cache database does not exist: $CACHE_DB" >&2
  exit 1
fi

IFS=',' read -r -a QUERY_IDS <<< "$TRACK_IDS"
TEMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TEMP_DIR"' EXIT

median_from_lines() {
  awk '
    { v[n++] = $1 }
    END {
      if (n == 0) {
        print "NaN"
        exit
      }
      for (i = 1; i < n; i++) {
        key = v[i]
        j = i - 1
        while (j >= 0 && v[j] > key) {
          v[j+1] = v[j]
          j--
        }
        v[j+1] = key
      }
      if (n % 2 == 1) {
        printf "%.6f\n", v[int(n/2)]
      } else {
        printf "%.6f\n", (v[n/2 - 1] + v[n/2]) / 2.0
      }
    }
  '
}

extract_timings() {
  local bin="$1"
  local benchmark="$2"

  "$bin" --root-dir "$TEMP_DIR" "$benchmark" 2>&1 | awk -F'\t' -v bm="$benchmark" '
    $1 == bm && $3 ~ /^[0-9]+(\.[0-9]+)?$/ { print $3 }
  '
}

prepare_local_benchmark() {
  local qid="$1"
  local out_file="$TEMP_DIR/benchmark/imdb/${qid}.benchmark"

  mkdir -p "$TEMP_DIR/benchmark/imdb"
  mkdir -p "$TEMP_DIR/duckdb_benchmark_data"
  ln -sf "$CACHE_DB" "$TEMP_DIR/duckdb_benchmark_data/imdb.duckdb"

  cat > "$out_file" <<EOF
# name: benchmark/imdb/${qid}.benchmark
# description: Local-cache JOB benchmark ${qid}
# group: [imdb]

name Q${qid}
group imdb

require parquet

cache imdb.duckdb

load ${REPO_ROOT}/benchmark/imdb/init/load.sql

run ${REPO_ROOT}/benchmark/imdb_plan_cost/queries/${qid}.sql

result ${REPO_ROOT}/benchmark/imdb/answers/${qid}.csv
EOF

  printf "%s\n" "benchmark/imdb/${qid}.benchmark"
}

mkdir -p "$(dirname "$OUTPUT_CSV")"

echo "query_id,baseline_median_s,candidate_median_s,delta_pct,improvement_pct,winner" > "$OUTPUT_CSV"

total_delta=0
improved=0
regressed=0
same=0
count=0

for qid in "${QUERY_IDS[@]}"; do
  benchmark="$(prepare_local_benchmark "$qid")"

  base_times="$(extract_timings "$BASELINE_BIN" "$benchmark")"
  cand_times="$(extract_timings "$CANDIDATE_BIN" "$benchmark")"

  if [[ -z "$base_times" || -z "$cand_times" ]]; then
    echo "Missing timings for $benchmark" >&2
    continue
  fi

  base_med="$(printf "%s\n" "$base_times" | median_from_lines)"
  cand_med="$(printf "%s\n" "$cand_times" | median_from_lines)"

  delta_pct="$(awk -v b="$base_med" -v c="$cand_med" 'BEGIN {
    if (b == 0) { printf "0.00" }
    else { printf "%.2f", ((c - b) / b) * 100.0 }
  }')"

  improvement_pct="$(awk -v b="$base_med" -v c="$cand_med" 'BEGIN {
    if (b == 0) { printf "0.00" }
    else { printf "%.2f", ((b - c) / b) * 100.0 }
  }')"

  winner="same"
  cmp="$(awk -v b="$base_med" -v c="$cand_med" 'BEGIN {
    if (c < b) print "candidate";
    else if (c > b) print "baseline";
    else print "same";
  }')"
  winner="$cmp"

  case "$winner" in
    candidate) improved=$((improved + 1)) ;;
    baseline) regressed=$((regressed + 1)) ;;
    same) same=$((same + 1)) ;;
  esac

  total_delta="$(awk -v t="$total_delta" -v d="$delta_pct" 'BEGIN { printf "%.6f", t + d }')"
  count=$((count + 1))

  echo "$qid,$base_med,$cand_med,$delta_pct,$improvement_pct,$winner" >> "$OUTPUT_CSV"
done

if [[ "$count" -eq 0 ]]; then
  echo "No queries were processed." >&2
  exit 1
fi

avg_delta="$(awk -v t="$total_delta" -v n="$count" 'BEGIN { printf "%.2f", t / n }')"

echo ""
echo "Completed comparison across $count queries"
echo "Average delta (candidate vs baseline): ${avg_delta}%"
echo "Improved: $improved | Regressed: $regressed | Same: $same"
echo "CSV: $OUTPUT_CSV"
