#!/usr/bin/env bash
set -euo pipefail

# Run "full" A2 + A3 cases (mixgraph + seek200 + worst seeks) using the
# latest dynamic mixgraph workload:
#   - hot key-range shifting (mix_hotset_enable + mix_shift_enable)
#   - cold scan burst injection (mix_burst_*)
#
# Defaults match experiment33 A-phase config:
#   - A2: cache=1GiB, threads=8
#   - A3: cache=2GiB, threads=8
#   - per-thread reads: mixgraph=50000, seek200=12500, worst1=25000, ...
#
# Usage:
#   bash tools/run_a2_a3_shift_burst_full.sh
#
# Tune knobs (recommended):
#   MIX_SHIFT_STAGE_SECONDS=30 MIX_BURST_INTERVAL_OPS=5000 MIX_BURST_SCAN_NEXTS=200 \
#     bash tools/run_a2_a3_shift_burst_full.sh

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNNER="$ROOT_DIR/tools/run_shortscan_compare.sh"
DB_BENCH="${DB_BENCH:-$ROOT_DIR/build/db_bench}"

if [[ ! -x "$RUNNER" ]]; then
  echo "missing runner: $RUNNER" >&2
  exit 1
fi
if [[ ! -x "$DB_BENCH" ]]; then
  echo "missing db_bench: $DB_BENCH" >&2
  exit 1
fi

EXP_DATE="${EXP_DATE:-$(date +%Y%m%d)}"
GLOBAL_EXP_ID="${GLOBAL_EXP_ID:-33}"

BASE_DB_DIR="${BASE_DB_DIR:-/tmp/rocksdb_base30g_vsz1024_lz4/db}"
if [[ ! -f "$BASE_DB_DIR/CURRENT" ]]; then
  echo "base DB not found: BASE_DB_DIR=$BASE_DB_DIR" >&2
  exit 2
fi

MATRIX_TAG="${MATRIX_TAG:-${EXP_DATE}_experiment${GLOBAL_EXP_ID}_A2A3_shift_burst_full}"
MATRIX_DIR="${MATRIX_DIR:-$ROOT_DIR/experiment/$MATRIX_TAG}"
mkdir -p "$MATRIX_DIR/cases"

# Try to reuse tmpfs root from the previous standard matrix if present.
TMPFS_ROOT="${TMPFS_ROOT:-}"
if [[ -z "$TMPFS_ROOT" ]]; then
  prev_tmpfs="$ROOT_DIR/experiment/20260220_experiment33_standard_matrix_30gb_simfs_l13_ro_cache_0p5_1_2/tmpfs_root.txt"
  if [[ -f "$prev_tmpfs" ]]; then
    TMPFS_ROOT="$(cat "$prev_tmpfs")"
  fi
fi

EXTRA_DB_BENCH_ARGS_ARRAY=()
if [[ -n "$TMPFS_ROOT" ]]; then
  if [[ ! -d "$TMPFS_ROOT" ]]; then
    echo "TMPFS_ROOT set but not found: $TMPFS_ROOT" >&2
    exit 3
  fi
  EXTRA_DB_BENCH_ARGS_ARRAY+=(
    --simulate_xp_nvm=1
    --simulate_xp_levels=1,3
    --simulate_xp_line_bytes=256
    --simulate_xp_buffer_bytes=16384
    --simulate_xp_latency_ns=300
    --simulate_xp_rpq_depth=64
    --simulate_xp_wpq_depth=64
    --simulate_xp_wpq_submit_ns=100
    --simulate_xp_prefetch_hit_ns=120
    --simulate_xp_enable_prefetch=true
    --simulate_xp_redirect_to_tmpfs=1
    --simulate_xp_tmpfs_root="$TMPFS_ROOT"
  )
fi
EXTRA_DB_BENCH_ARGS_ARRAY+=(--readonly=1 --disable_auto_compactions=1)

extra_args_joined=""
if (( ${#EXTRA_DB_BENCH_ARGS_ARRAY[@]} > 0 )); then
  extra_args_joined="${EXTRA_DB_BENCH_ARGS_ARRAY[*]}"
fi

# Common workload config (match experiment33 defaults unless overridden).
THREADS="${THREADS:-8}"
KEY_SIZE="${KEY_SIZE:-16}"
VALUE_SIZE="${VALUE_SIZE:-1024}"
COMPRESSION_TYPE="${COMPRESSION_TYPE:-lz4}"
USE_DIRECT="${USE_DIRECT:-true}"
NUM_KEYS="${NUM_KEYS:-30973321}"

REALISTIC_READS="${REALISTIC_READS:-50000}"
STEP200_READS="${STEP200_READS:-12500}"
WORST_READS_1="${WORST_READS_1:-25000}"
WORST_READS_4="${WORST_READS_4:-12500}"
WORST_READS_20="${WORST_READS_20:-6250}"
WORST_READS_200="${WORST_READS_200:-2500}"
WORST_READS_10000="${WORST_READS_10000:-250}"

MIX_GET_RATIO="${MIX_GET_RATIO:-0.20}"
MIX_PUT_RATIO="${MIX_PUT_RATIO:-0.00}"
MIX_SEEK_RATIO="${MIX_SEEK_RATIO:-0.80}"

MIX_KEY_DIST_A="${MIX_KEY_DIST_A:-0.0016}"
MIX_KEY_DIST_B="${MIX_KEY_DIST_B:--0.71}"
MIX_KEYRANGE_DIST_A="${MIX_KEYRANGE_DIST_A:-14.18}"
MIX_KEYRANGE_DIST_B="${MIX_KEYRANGE_DIST_B:--2.917}"
MIX_KEYRANGE_DIST_C="${MIX_KEYRANGE_DIST_C:-0.0164}"
MIX_KEYRANGE_DIST_D="${MIX_KEYRANGE_DIST_D:--0.08082}"
MIX_KEYRANGE_NUM="${MIX_KEYRANGE_NUM:-32}"
MIX_ITER_K="${MIX_ITER_K:-0.08}"
MIX_ITER_SIGMA="${MIX_ITER_SIGMA:-1.75}"
MIX_ITER_THETA="${MIX_ITER_THETA:-0}"

# Enable dynamic hot ranges.
MIX_HOTSET_ENABLE="${MIX_HOTSET_ENABLE:-1}"
MIX_HOTSET_RANGE_PCT="${MIX_HOTSET_RANGE_PCT:-0.03}"
MIX_HOTSET_RANGE_ACCESS_PCT="${MIX_HOTSET_RANGE_ACCESS_PCT:-0.88}"
MIX_HOTSET_RANGE_ZIPF_THETA="${MIX_HOTSET_RANGE_ZIPF_THETA:-1.0}"
MIX_HOTSET_KEY_PCT="${MIX_HOTSET_KEY_PCT:-0.01}"
MIX_HOTSET_KEY_ACCESS_PCT="${MIX_HOTSET_KEY_ACCESS_PCT:-0.80}"
MIX_HOTSET_EVENLY_SPREAD_RANGES="${MIX_HOTSET_EVENLY_SPREAD_RANGES:-1}"

MIX_SHIFT_ENABLE="${MIX_SHIFT_ENABLE:-1}"
MIX_SHIFT_MODE="${MIX_SHIFT_MODE:-step_jump}"
MIX_SHIFT_STAGE_SECONDS="${MIX_SHIFT_STAGE_SECONDS:-30}"
MIX_SHIFT_STRIDE_RANGES="${MIX_SHIFT_STRIDE_RANGES:-1}"
MIX_SHIFT_JUMP_MULTIPLIER="${MIX_SHIFT_JUMP_MULTIPLIER:-4}"
MIX_SHIFT_BASE_START_RANGE="${MIX_SHIFT_BASE_START_RANGE:-0}"
MIX_SHIFT_LOG_STAGE_TRANSITIONS="${MIX_SHIFT_LOG_STAGE_TRANSITIONS:-0}"

# Enable bursty cold scan injection.
MIX_BURST_ENABLE="${MIX_BURST_ENABLE:-1}"
MIX_BURST_INTERVAL_OPS="${MIX_BURST_INTERVAL_OPS:-5000}"
MIX_BURST_SCAN_NEXTS="${MIX_BURST_SCAN_NEXTS:-200}"
MIX_BURST_COLD_RANGES_ONLY="${MIX_BURST_COLD_RANGES_ONLY:-1}"
MIX_BURST_LOG="${MIX_BURST_LOG:-0}"

run_case() {
  local label="$1"
  local cache_bytes="$2"

  local exp_name="experiment${GLOBAL_EXP_ID}_${label}_cache_sweep_seekheavy_shift_burst"
  local exp_dir="$MATRIX_DIR/cases/${label}_cache_sweep_seekheavy_shift_burst"
  local run_tag="${EXP_DATE}_exp${GLOBAL_EXP_ID}_${exp_name}_$(date +%H%M%S)"
  local out_dir="$exp_dir/run_results/$run_tag"

  mkdir -p "$exp_dir"
  echo "[a2a3] START $label cache_bytes=$cache_bytes out_dir=$out_dir"

  RUN_TAG="$run_tag" \
  EXP_DATE="$EXP_DATE" \
  GLOBAL_EXP_ID="$GLOBAL_EXP_ID" \
  EXPERIMENT_NAME="$exp_name" \
  EXPERIMENT_DIR="$exp_dir" \
  OUT_DIR="$out_dir" \
  DB_BENCH="$DB_BENCH" \
  PROFILE=s \
  SCALE=1.0 \
  NUM_KEYS="$NUM_KEYS" \
  REALISTIC_READS="$REALISTIC_READS" \
  STEP200_READS="$STEP200_READS" \
  WORST_READS_1="$WORST_READS_1" \
  WORST_READS_4="$WORST_READS_4" \
  WORST_READS_20="$WORST_READS_20" \
  WORST_READS_200="$WORST_READS_200" \
  WORST_READS_10000="$WORST_READS_10000" \
  CACHE_SIZES="$cache_bytes" \
  THREADS="$THREADS" \
  FILL_THREADS="$THREADS" \
  KEY_SIZE="$KEY_SIZE" \
  VALUE_SIZE="$VALUE_SIZE" \
  COMPRESSION_TYPE="$COMPRESSION_TYPE" \
  USE_DIRECT="$USE_DIRECT" \
  SKIP_FILL=1 \
  ISOLATE_BY_CACHE=0 \
  RUN_ONLY_MIXGRAPH=0 \
  AUTO_POST_PROCESS=1 \
  MIX_GET_RATIO="$MIX_GET_RATIO" \
  MIX_PUT_RATIO="$MIX_PUT_RATIO" \
  MIX_SEEK_RATIO="$MIX_SEEK_RATIO" \
  MIX_KEY_DIST_A="$MIX_KEY_DIST_A" \
  MIX_KEY_DIST_B="$MIX_KEY_DIST_B" \
  MIX_KEYRANGE_DIST_A="$MIX_KEYRANGE_DIST_A" \
  MIX_KEYRANGE_DIST_B="$MIX_KEYRANGE_DIST_B" \
  MIX_KEYRANGE_DIST_C="$MIX_KEYRANGE_DIST_C" \
  MIX_KEYRANGE_DIST_D="$MIX_KEYRANGE_DIST_D" \
  MIX_KEYRANGE_NUM="$MIX_KEYRANGE_NUM" \
  MIX_HOTSET_ENABLE="$MIX_HOTSET_ENABLE" \
  MIX_HOTSET_RANGE_PCT="$MIX_HOTSET_RANGE_PCT" \
  MIX_HOTSET_RANGE_ACCESS_PCT="$MIX_HOTSET_RANGE_ACCESS_PCT" \
  MIX_HOTSET_RANGE_ZIPF_THETA="$MIX_HOTSET_RANGE_ZIPF_THETA" \
  MIX_HOTSET_KEY_PCT="$MIX_HOTSET_KEY_PCT" \
  MIX_HOTSET_KEY_ACCESS_PCT="$MIX_HOTSET_KEY_ACCESS_PCT" \
  MIX_HOTSET_EVENLY_SPREAD_RANGES="$MIX_HOTSET_EVENLY_SPREAD_RANGES" \
  MIX_SHIFT_ENABLE="$MIX_SHIFT_ENABLE" \
  MIX_SHIFT_MODE="$MIX_SHIFT_MODE" \
  MIX_SHIFT_STAGE_SECONDS="$MIX_SHIFT_STAGE_SECONDS" \
  MIX_SHIFT_STRIDE_RANGES="$MIX_SHIFT_STRIDE_RANGES" \
  MIX_SHIFT_JUMP_MULTIPLIER="$MIX_SHIFT_JUMP_MULTIPLIER" \
  MIX_SHIFT_BASE_START_RANGE="$MIX_SHIFT_BASE_START_RANGE" \
  MIX_SHIFT_LOG_STAGE_TRANSITIONS="$MIX_SHIFT_LOG_STAGE_TRANSITIONS" \
  MIX_ITER_K="$MIX_ITER_K" \
  MIX_ITER_SIGMA="$MIX_ITER_SIGMA" \
  MIX_ITER_THETA="$MIX_ITER_THETA" \
  MIX_BURST_ENABLE="$MIX_BURST_ENABLE" \
  MIX_BURST_INTERVAL_OPS="$MIX_BURST_INTERVAL_OPS" \
  MIX_BURST_SCAN_NEXTS="$MIX_BURST_SCAN_NEXTS" \
  MIX_BURST_COLD_RANGES_ONLY="$MIX_BURST_COLD_RANGES_ONLY" \
  MIX_BURST_LOG="$MIX_BURST_LOG" \
  DB_DIR="$BASE_DB_DIR" \
  WAL_DIR="/tmp/rocksdb_${MATRIX_TAG}_${label}_wal" \
  EXTRA_DB_BENCH_ARGS="$extra_args_joined" \
  bash "$RUNNER"

  echo "[a2a3] END   $label out_dir=$out_dir"
  echo "[a2a3] mixgraph cache stats ($label):"
  rg -n "rocksdb\\.block\\.cache\\.data\\.(miss|hit|bytes\\.insert) COUNT" "$out_dir"/02_mixgraph_cache_*.log || true
}

run_case "A2" "$((1<<30))"
run_case "A3" "$((2<<30))"
