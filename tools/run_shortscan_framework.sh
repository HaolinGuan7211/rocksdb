#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="$ROOT_DIR/tools/run_shortscan_compare.sh"
AGG_SCRIPT="$ROOT_DIR/tools/aggregate_shortscan_runs.py"

if [[ ! -x "$RUN_SCRIPT" ]]; then
  echo "missing run script: $RUN_SCRIPT" >&2
  exit 1
fi

FRAMEWORK_TAG="${FRAMEWORK_TAG:-$(date +%Y%m%d_%H%M%S)}"
REPEATS="${REPEATS:-3}"
EXPERIMENT_ROOT="${EXPERIMENT_ROOT:-$ROOT_DIR/experiment}"
RUNS_ROOT="${RUNS_ROOT:-$EXPERIMENT_ROOT/$FRAMEWORK_TAG/run_results}"
AGG_ROOT="${AGG_ROOT:-$EXPERIMENT_ROOT/$FRAMEWORK_TAG/framework_aggregate}"
mkdir -p "$AGG_ROOT"
mkdir -p "$RUNS_ROOT"

run_dirs=()
for i in $(seq 1 "$REPEATS"); do
  run_tag="${FRAMEWORK_TAG}_r${i}"
  echo "[framework] start repeat $i/$REPEATS -> $run_tag"

  OUT_DIR="$RUNS_ROOT/$run_tag" RUN_TAG="$run_tag" "$RUN_SCRIPT"

  run_dir="$RUNS_ROOT/$run_tag"
  run_dirs+=("$run_dir")
done

agg_args=()
for d in "${run_dirs[@]}"; do
  agg_args+=(--run-dir "$d")
done
python3 "$AGG_SCRIPT" "${agg_args[@]}" --out-dir "$AGG_ROOT"

cat >"$AGG_ROOT/README.md" <<EOF
# Shortscan Framework Output

- framework_tag: $FRAMEWORK_TAG
- repeats: $REPEATS
- run_dirs:
$(printf '  - %s\n' "${run_dirs[@]}")

Files:
- aggregate_metrics.csv
- mixgraph_median_ops_vs_cache.png
- mixgraph_median_seek_p99_vs_cache.png
EOF

echo "[framework] done. aggregate dir: $AGG_ROOT"
