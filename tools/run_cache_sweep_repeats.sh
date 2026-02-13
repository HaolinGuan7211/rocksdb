#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MATRIX_SCRIPT="$ROOT_DIR/tools/run_standard_matrix_50gb.sh"
PLOT_SCRIPT="$ROOT_DIR/tools/plot_cache_sweep_repeats.py"

if [[ ! -x "$MATRIX_SCRIPT" ]]; then
  echo "missing script: $MATRIX_SCRIPT" >&2
  exit 1
fi
if [[ ! -f "$PLOT_SCRIPT" ]]; then
  echo "missing script: $PLOT_SCRIPT" >&2
  exit 1
fi

EXP_DATE="${EXP_DATE:-$(date +%Y%m%d)}"
REPEATS="${REPEATS:-3}"
START_EXPERIMENT_ID="${START_EXPERIMENT_ID:-${START_EXP_ID:-0}}"
CACHE_SWEEP_GIBS="${CACHE_SWEEP_GIBS:-1,2,4}"
TARGET_DB_GB="${TARGET_DB_GB:-50}"
EXPERIMENT_ROOT="${EXPERIMENT_ROOT:-$ROOT_DIR/experiment}"
BASE_DB_ROOT="${BASE_DB_ROOT:-/tmp/rocksdb_sst_ingest_base_20260205_exp10_sst_ingest50gb_120546}"
BASE_WAL_ROOT="${BASE_WAL_ROOT:-/tmp/rocksdb_sst_ingest_base_20260205_exp10_sst_ingest50gb_120546_wal}"
REUSE_DB_MODE="${REUSE_DB_MODE:-shared}"
SKIP_IF_EXISTS="${SKIP_IF_EXISTS:-1}"

if [[ "$REPEATS" -le 0 ]]; then
  echo "REPEATS must be > 0" >&2
  exit 1
fi

if [[ ! -f "$BASE_DB_ROOT/db/CURRENT" ]]; then
  echo "base db not found: $BASE_DB_ROOT/db/CURRENT" >&2
  exit 1
fi

REPEAT_TAG="${EXP_DATE}_experiment${START_EXPERIMENT_ID}_cache_sweep_repeats_50gb"
REPEAT_DIR="$EXPERIMENT_ROOT/$REPEAT_TAG"
REGISTRY="$REPEAT_DIR/repeat_registry.csv"
PLAN_MD="$REPEAT_DIR/repeat_plan.md"
RUNNER_LOG="$REPEAT_DIR/repeat_runner.log"
ANALYSIS_DIR="$REPEAT_DIR/analysis"

mkdir -p "$REPEAT_DIR" "$ANALYSIS_DIR"

cat >"$PLAN_MD" <<EOF
# Cache Sweep Repeat Plan (50GB)

- exp_date: $EXP_DATE
- start_experiment_id: $START_EXPERIMENT_ID
- repeats: $REPEATS
- cache_candidates_gib: $CACHE_SWEEP_GIBS
- target_db_gb: $TARGET_DB_GB
- objective: reduce one-shot noise by repeated runs with randomized cache order, then report median+std.
- base_db_root: $BASE_DB_ROOT
- reuse_db_mode: $REUSE_DB_MODE
EOF

cat >"$REGISTRY" <<EOF
repeat_idx,experiment_id,matrix_tag,matrix_dir,cache_order_gib,status
EOF

permute_csv() {
  local csv="$1"
  python3 - "$csv" <<'PY'
import random
import sys
vals=[x.strip() for x in sys.argv[1].split(",") if x.strip()]
random.shuffle(vals)
print(",".join(vals))
PY
}

matrix_dirs=()
current_experiment_id="$START_EXPERIMENT_ID"

for ((i=1; i<=REPEATS; i++)); do
  perm="$(permute_csv "$CACHE_SWEEP_GIBS")"
  matrix_tag="${EXP_DATE}_experiment${current_experiment_id}_cache_sweep_50gb_r${i}"
  matrix_dir="$EXPERIMENT_ROOT/$matrix_tag"

  echo "[$(date '+%F %T')] repeat=$i experiment_id=$current_experiment_id cache_order=$perm matrix_tag=$matrix_tag" | tee -a "$RUNNER_LOG"

  EXP_DATE="$EXP_DATE" \
  EXPERIMENT_ID="$current_experiment_id" \
  MATRIX_NAME="cache_sweep_50gb_r${i}" \
  MATRIX_TAG="$matrix_tag" \
  MATRIX_DIR="$matrix_dir" \
  TARGET_DB_GB="$TARGET_DB_GB" \
  CACHE_SWEEP_GIBS="$perm" \
  MAX_RUNS=3 \
  PREPARE_BASE_DB=0 \
  BASE_DB_ROOT="$BASE_DB_ROOT" \
  BASE_WAL_ROOT="$BASE_WAL_ROOT" \
  REUSE_DB_MODE="$REUSE_DB_MODE" \
  SKIP_IF_EXISTS="$SKIP_IF_EXISTS" \
  "$MATRIX_SCRIPT" 2>&1 | tee -a "$RUNNER_LOG"

  echo "$i,$current_experiment_id,$matrix_tag,$matrix_dir,\"$perm\",done" >>"$REGISTRY"
  matrix_dirs+=("$matrix_dir")
  current_experiment_id=$((current_experiment_id + 1))
done

plot_args=()
for d in "${matrix_dirs[@]}"; do
  plot_args+=(--matrix-dir "$d")
done

python3 "$PLOT_SCRIPT" "${plot_args[@]}" --out-dir "$ANALYSIS_DIR" | tee -a "$RUNNER_LOG"

echo "done: $REPEAT_DIR"
