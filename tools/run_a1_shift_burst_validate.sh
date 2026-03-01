#!/usr/bin/env bash
set -euo pipefail

# One-click validator for:
#  - hot key-range shifting in mixgraph (mix_hotset_enable + mix_shift_enable)
#  - bursty scan traffic that intentionally targets cold ranges (mix_burst_*)
#
# This script runs ONLY step 02 (mixgraph) using tools/run_shortscan_compare.sh
# and prints block-cache miss/hit + bytes.insert at the end.
#
# Typical usage:
#   bash tools/run_a1_shift_burst_validate.sh
#
# Override knobs (recommended):
#   MIX_SHIFT_STAGE_SECONDS=30 MIX_BURST_INTERVAL_OPS=5000 MIX_BURST_SCAN_NEXTS=200 \
#     bash tools/run_a1_shift_burst_validate.sh
#
# Point to your existing 30GB base DB + tmpfs staging:
#   DB_DIR=/tmp/rocksdb_base30g_vsz1024_lz4/db TMPFS_ROOT=/dev/shm/nvm_tmpfs_root.XXXX \
#     bash tools/run_a1_shift_burst_validate.sh

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
EXPERIMENT_NAME="${EXPERIMENT_NAME:-A1_shift_burst_validate}"
RUN_TIME="${RUN_TIME:-$(date +%H%M%S)}"
RUN_TAG="${RUN_TAG:-${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}_${RUN_TIME}}"
EXPERIMENT_DIR="${EXPERIMENT_DIR:-$ROOT_DIR/experiment/${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}}"
OUT_DIR="${OUT_DIR:-$EXPERIMENT_DIR/run_results/$RUN_TAG}"

# A1 baseline (match experiment33 defaults unless overridden).
THREADS="${THREADS:-8}"
KEY_SIZE="${KEY_SIZE:-16}"
VALUE_SIZE="${VALUE_SIZE:-1024}"
COMPRESSION_TYPE="${COMPRESSION_TYPE:-lz4}"
USE_DIRECT="${USE_DIRECT:-true}"
NUM_KEYS="${NUM_KEYS:-30973321}"       # ~30GB raw target for key=16,value=1024
REALISTIC_READS="${REALISTIC_READS:-50000}"  # per-thread reads
CACHE_SIZES="${CACHE_SIZES:-536870912}"      # 0.5GiB

# Existing base DB (must already exist).
DB_DIR="${DB_DIR:-/tmp/rocksdb_base30g_vsz1024_lz4/db}"
if [[ ! -f "$DB_DIR/CURRENT" ]]; then
  echo "base DB not found (set DB_DIR to an existing RocksDB): $DB_DIR" >&2
  exit 2
fi

# Simulated NVM / tmpfs redirect (optional, but recommended to match your matrix).
TMPFS_ROOT="${TMPFS_ROOT:-}"
if [[ -z "$TMPFS_ROOT" ]]; then
  # Best-effort: if the previous matrix dir exists, reuse its recorded tmpfs root.
  prev_tmpfs="$ROOT_DIR/experiment/20260220_experiment33_standard_matrix_30gb_simfs_l13_ro_cache_0p5_1_2/tmpfs_root.txt"
  if [[ -f "$prev_tmpfs" ]]; then
    TMPFS_ROOT="$(cat "$prev_tmpfs")"
  fi
fi

EXTRA_DB_BENCH_ARGS_ARRAY=()
if [[ -n "$TMPFS_ROOT" ]]; then
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

# Keep DB static for cleaner cache-hit interpretation.
EXTRA_DB_BENCH_ARGS_ARRAY+=(--readonly=1 --disable_auto_compactions=1)

# Mixgraph ratios (seek-heavy by default).
MIX_GET_RATIO="${MIX_GET_RATIO:-0.20}"
MIX_PUT_RATIO="${MIX_PUT_RATIO:-0.00}"
MIX_SEEK_RATIO="${MIX_SEEK_RATIO:-0.80}"

# Locality parameters (same as your matrix defaults).
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

# Enable hotset + time-based shifting.
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

# Bursty cold scan injection (to create transient misses).
MIX_BURST_ENABLE="${MIX_BURST_ENABLE:-1}"
MIX_BURST_INTERVAL_OPS="${MIX_BURST_INTERVAL_OPS:-5000}"
MIX_BURST_SCAN_NEXTS="${MIX_BURST_SCAN_NEXTS:-200}"
MIX_BURST_COLD_RANGES_ONLY="${MIX_BURST_COLD_RANGES_ONLY:-1}"
MIX_BURST_LOG="${MIX_BURST_LOG:-0}"

# Monitoring / probes (new monitoring体系 validation).
MIX_MONITOR_ENABLE="${MIX_MONITOR_ENABLE:-1}"
MIX_MONITOR_WINDOW_US="${MIX_MONITOR_WINDOW_US:-1000000}"
MIX_PROBE_ENABLE="${MIX_PROBE_ENABLE:-1}"
MIX_PROBE_INTERVAL_OPS="${MIX_PROBE_INTERVAL_OPS:-5000}"
MIX_PROBE_READS="${MIX_PROBE_READS:-64}"

SIMFS_MONITOR_ENABLE="${SIMFS_MONITOR_ENABLE:-1}"
SIMFS_MONITOR_WINDOW_US="${SIMFS_MONITOR_WINDOW_US:-1000000}"
SIMFS_MONITOR_STAGE_SECONDS="${SIMFS_MONITOR_STAGE_SECONDS:-$MIX_SHIFT_STAGE_SECONDS}"
SIMFS_MONITOR_MAX_READ="${SIMFS_MONITOR_MAX_READ:-1}"
SIMFS_MONITOR_MAX_OPEN="${SIMFS_MONITOR_MAX_OPEN:-1}"
SIMFS_MONITOR_MAX_PREFETCH="${SIMFS_MONITOR_MAX_PREFETCH:-1}"

mkdir -p "$OUT_DIR"

extra_args_joined=""
if (( ${#EXTRA_DB_BENCH_ARGS_ARRAY[@]} > 0 )); then
  extra_args_joined="${EXTRA_DB_BENCH_ARGS_ARRAY[*]}"
fi

echo "[a1-validate] OUT_DIR=$OUT_DIR"
echo "[a1-validate] DB_DIR=$DB_DIR"
if [[ -n "$TMPFS_ROOT" ]]; then
  echo "[a1-validate] TMPFS_ROOT=$TMPFS_ROOT"
else
  echo "[a1-validate] TMPFS_ROOT=(not set; simulated tmpfs redirect disabled)"
fi
echo "[a1-validate] shift: stage_seconds=$MIX_SHIFT_STAGE_SECONDS mode=$MIX_SHIFT_MODE"
echo "[a1-validate] burst: interval_ops=$MIX_BURST_INTERVAL_OPS scan_nexts=$MIX_BURST_SCAN_NEXTS"
echo "[a1-validate] monitor: mix_monitor_enable=$MIX_MONITOR_ENABLE simfs_monitor_enable=$SIMFS_MONITOR_ENABLE"

RUN_TAG="$RUN_TAG" \
EXP_DATE="$EXP_DATE" \
GLOBAL_EXP_ID="$GLOBAL_EXP_ID" \
EXPERIMENT_NAME="$EXPERIMENT_NAME" \
RUN_TIME="$RUN_TIME" \
EXPERIMENT_DIR="$EXPERIMENT_DIR" \
OUT_DIR="$OUT_DIR" \
DB_BENCH="$DB_BENCH" \
PROFILE=s \
SCALE=1.0 \
NUM_KEYS="$NUM_KEYS" \
REALISTIC_READS="$REALISTIC_READS" \
CACHE_SIZES="$CACHE_SIZES" \
THREADS="$THREADS" \
FILL_THREADS="$THREADS" \
KEY_SIZE="$KEY_SIZE" \
VALUE_SIZE="$VALUE_SIZE" \
COMPRESSION_TYPE="$COMPRESSION_TYPE" \
USE_DIRECT="$USE_DIRECT" \
SKIP_FILL=1 \
ISOLATE_BY_CACHE=0 \
RUN_ONLY_MIXGRAPH=1 \
AUTO_POST_PROCESS="${AUTO_POST_PROCESS:-1}" \
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
MIX_MONITOR_ENABLE="$MIX_MONITOR_ENABLE" \
MIX_MONITOR_WINDOW_US="$MIX_MONITOR_WINDOW_US" \
MIX_PROBE_ENABLE="$MIX_PROBE_ENABLE" \
MIX_PROBE_INTERVAL_OPS="$MIX_PROBE_INTERVAL_OPS" \
MIX_PROBE_READS="$MIX_PROBE_READS" \
SIMFS_MONITOR_ENABLE="$SIMFS_MONITOR_ENABLE" \
SIMFS_MONITOR_WINDOW_US="$SIMFS_MONITOR_WINDOW_US" \
SIMFS_MONITOR_STAGE_SECONDS="$SIMFS_MONITOR_STAGE_SECONDS" \
SIMFS_MONITOR_MAX_READ="$SIMFS_MONITOR_MAX_READ" \
SIMFS_MONITOR_MAX_OPEN="$SIMFS_MONITOR_MAX_OPEN" \
SIMFS_MONITOR_MAX_PREFETCH="$SIMFS_MONITOR_MAX_PREFETCH" \
DB_DIR="$DB_DIR" \
WAL_DIR="/tmp/rocksdb_${RUN_TAG}_wal" \
EXTRA_DB_BENCH_ARGS="$extra_args_joined" \
bash "$RUNNER"

mix_log="$(ls -1 "$OUT_DIR"/02_mixgraph_cache_*.log | head -n 1)"
echo
echo "[a1-validate] mixgraph log: $mix_log"
echo "[a1-validate] cache stats:"
rg -n "rocksdb\\.block\\.cache\\.data\\.(miss|hit|bytes\\.insert) COUNT" "$mix_log" || true

if [[ "${MIX_MONITOR_ENABLE:-0}" == "1" || "${SIMFS_MONITOR_ENABLE:-0}" == "1" ]]; then
  echo
  echo "[a1-validate] generating monitoring figures/reports..."
  python3 "$ROOT_DIR/tools/plot_mixgraph_monitoring.py" --run_dir "$OUT_DIR" || true
fi
