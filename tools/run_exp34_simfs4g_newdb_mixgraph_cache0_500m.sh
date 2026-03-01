#!/usr/bin/env bash
set -euo pipefail

# Exp34: create a fresh ~4GiB DB and run mixgraph (seek-heavy) under simfs.
# Two cache tiers:
#   - cache_size=0 (effectively block cache off)
#   - cache_size=500MB
#
# Key requirement: mixgraph Seek should not miss (use fillseq, not fillrandom).
#
# Usage:
#   bash tools/run_exp34_simfs4g_newdb_mixgraph_cache0_500m.sh
#
# Common overrides:
#   TARGET_DB_GIB=4 VALUE_SIZE=1024 THREADS=8 REALISTIC_READS=200000 \
#     bash tools/run_exp34_simfs4g_newdb_mixgraph_cache0_500m.sh
#
#   RESET_DB=1 TMPFS_ROOT=/dev/shm/nvm_tmpfs_root.exp34_simfs4g \
#     bash tools/run_exp34_simfs4g_newdb_mixgraph_cache0_500m.sh

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
GLOBAL_EXP_ID="${GLOBAL_EXP_ID:-34}"
EXPERIMENT_NAME="${EXPERIMENT_NAME:-simfs4g_newdb_mixgraph_cache0_500m}"
RUN_TIME="${RUN_TIME:-$(date +%H%M%S)}"
RUN_TAG="${RUN_TAG:-${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}_${RUN_TIME}}"
EXPERIMENT_DIR="${EXPERIMENT_DIR:-$ROOT_DIR/experiment/${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}}"
OUT_DIR="${OUT_DIR:-$EXPERIMENT_DIR/run_results/$RUN_TAG}"

# DB shape: target ~4GiB raw, adjust via TARGET_DB_GIB.
TARGET_DB_GIB="${TARGET_DB_GIB:-4}"
KEY_SIZE="${KEY_SIZE:-16}"
VALUE_SIZE="${VALUE_SIZE:-1024}"
COMPRESSION_TYPE="${COMPRESSION_TYPE:-lz4}"

# Workload knobs (seek-heavy mixgraph like your A1).
THREADS="${THREADS:-8}"
FILL_THREADS="${FILL_THREADS:-$THREADS}"
REALISTIC_READS="${REALISTIC_READS:-100000}" # per-thread

MIX_GET_RATIO="${MIX_GET_RATIO:-0.20}"
MIX_PUT_RATIO="${MIX_PUT_RATIO:-0.00}"
MIX_SEEK_RATIO="${MIX_SEEK_RATIO:-0.80}"

# Locality parameters (reuse your experiment33 defaults).
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

# Hotset + shifting + burst knobs (enabled by default; override if you want pure steady).
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

MIX_BURST_ENABLE="${MIX_BURST_ENABLE:-1}"
MIX_BURST_INTERVAL_OPS="${MIX_BURST_INTERVAL_OPS:-5000}"
MIX_BURST_SCAN_NEXTS="${MIX_BURST_SCAN_NEXTS:-200}"
MIX_BURST_COLD_RANGES_ONLY="${MIX_BURST_COLD_RANGES_ONLY:-1}"

# Two cache tiers: OFF (0) and 500MB.
CACHE_SIZES="${CACHE_SIZES:-0,536870912}"

# DB paths (kept stable by default so you can reuse between runs if RESET_DB=0).
DB_DIR="${DB_DIR:-/tmp/rocksdb_simfs4g_vsz1024_lz4/db}"
WAL_DIR="${WAL_DIR:-/tmp/rocksdb_simfs4g_vsz1024_lz4/wal}"

# Optional: redirect simfs-managed files to a tmpfs root to reduce base FS noise.
# Disabled by default because /dev/shm is often smaller than 4GiB in this env.
USE_TMPFS_REDIRECT="${USE_TMPFS_REDIRECT:-0}" # 0|1
TMPFS_ROOT="${TMPFS_ROOT:-/dev/shm/nvm_tmpfs_root.exp34_simfs4g}"

# Reset behavior.
RESET_DB="${RESET_DB:-1}"

# If 1, reuse existing DB (skip step-01 fill). This is useful for re-running
# mixgraph with different sampling parameters without spending ~15min refilling.
SKIP_FILL="${SKIP_FILL:-0}"

# When SKIP_FILL=1, you can also bound mixgraph by wall-clock time via --duration
# so high-QPS cases (e.g. 500MB cache) don't finish in just a few seconds and
# produce tiny/odd-looking monitoring time series.
MIXGRAPH_DURATION_SECONDS="${MIXGRAPH_DURATION_SECONDS:-0}"

# Monitoring + histogram for P50/P95/P99 extraction.
MIX_MONITOR_ENABLE="${MIX_MONITOR_ENABLE:-1}"
SIMFS_MONITOR_ENABLE="${SIMFS_MONITOR_ENABLE:-1}"
MIX_MONITOR_WINDOW_US="${MIX_MONITOR_WINDOW_US:-1000000}"
SIMFS_MONITOR_WINDOW_US="${SIMFS_MONITOR_WINDOW_US:-1000000}"
SIMFS_MONITOR_STAGE_SECONDS="${SIMFS_MONITOR_STAGE_SECONDS:-$MIX_SHIFT_STAGE_SECONDS}"

mkdir -p "$OUT_DIR" "$EXPERIMENT_DIR"

NUM_KEYS="$(awk -v gib="$TARGET_DB_GIB" -v k="$KEY_SIZE" -v v="$VALUE_SIZE" \
  'BEGIN { bytes=gib*1024*1024*1024; per=k+v; n=int(bytes/per); if(n<1)n=1; print n }')"

echo "[exp34] OUT_DIR=$OUT_DIR"
echo "[exp34] DB_DIR=$DB_DIR"
echo "[exp34] WAL_DIR=$WAL_DIR"
if [[ "$USE_TMPFS_REDIRECT" == "1" ]]; then
  echo "[exp34] TMPFS_ROOT=$TMPFS_ROOT (redirect enabled)"
else
  echo "[exp34] TMPFS_ROOT=(redirect disabled)"
fi
echo "[exp34] target_db_gib=$TARGET_DB_GIB key_size=$KEY_SIZE value_size=$VALUE_SIZE -> num_keys=$NUM_KEYS"
echo "[exp34] cache_sizes=$CACHE_SIZES"

if [[ "$RESET_DB" == "1" ]]; then
  echo "[exp34] RESET_DB=1: clearing DB_DIR/WAL_DIR"
  rm -rf "$DB_DIR" "$WAL_DIR"
  if [[ "$USE_TMPFS_REDIRECT" == "1" ]]; then
    echo "[exp34] clearing TMPFS_ROOT=$TMPFS_ROOT"
    rm -rf "$TMPFS_ROOT"
  fi
fi
mkdir -p "$(dirname "$DB_DIR")" "$(dirname "$WAL_DIR")" "$WAL_DIR"

# Simulated hybrid FS: put ALL levels under simulated NVM and redirect NVM paths to tmpfs,
# so base FS noise is minimized and simfs is the dominant path.
EXTRA_DB_BENCH_ARGS_ARRAY=(
  --histogram=1
  --disable_wal=1
  --simulate_xp_nvm=1
  --simulate_xp_levels=0,1,2,3,4,5,6
  --simulate_xp_line_bytes=256
  --simulate_xp_buffer_bytes=16384
  --simulate_xp_latency_ns=300
  --simulate_xp_rpq_depth=64
  --simulate_xp_wpq_depth=64
  --simulate_xp_wpq_submit_ns=100
  --simulate_xp_prefetch_hit_ns=120
  --simulate_xp_enable_prefetch=true
)

if [[ "$USE_TMPFS_REDIRECT" == "1" ]]; then
  EXTRA_DB_BENCH_ARGS_ARRAY+=(
    --simulate_xp_redirect_to_tmpfs=1
    --simulate_xp_tmpfs_root="$TMPFS_ROOT"
  )
fi

# Apply --duration only when reusing an existing DB. Otherwise it could truncate
# the fill benchmark.
if [[ "$SKIP_FILL" == "1" && "$MIXGRAPH_DURATION_SECONDS" != "0" ]]; then
  EXTRA_DB_BENCH_ARGS_ARRAY+=(--duration="$MIXGRAPH_DURATION_SECONDS")
fi

EXTRA_DB_BENCH_ARGS="${EXTRA_DB_BENCH_ARGS:-${EXTRA_DB_BENCH_ARGS_ARRAY[*]}}"

# Run: fillseq to guarantee all keys exist => Seek shouldn't be empty.
RUN_TAG="$RUN_TAG" \
EXPERIMENT_DIR="$EXPERIMENT_DIR" \
OUT_DIR="$OUT_DIR" \
DB_BENCH="$DB_BENCH" \
PROFILE=smoke \
SCALE=1.0 \
NUM_KEYS="$NUM_KEYS" \
REALISTIC_READS="$REALISTIC_READS" \
CACHE_SIZES="$CACHE_SIZES" \
THREADS="$THREADS" \
FILL_THREADS="$FILL_THREADS" \
KEY_SIZE="$KEY_SIZE" \
VALUE_SIZE="$VALUE_SIZE" \
COMPRESSION_TYPE="$COMPRESSION_TYPE" \
USE_DIRECT=true \
SKIP_FILL="$SKIP_FILL" \
FILL_BENCHMARK=fillseq \
ISOLATE_BY_CACHE=0 \
RUN_ONLY_MIXGRAPH=1 \
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
MIX_ITER_K="$MIX_ITER_K" \
MIX_ITER_SIGMA="$MIX_ITER_SIGMA" \
MIX_ITER_THETA="$MIX_ITER_THETA" \
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
MIX_BURST_ENABLE="$MIX_BURST_ENABLE" \
MIX_BURST_INTERVAL_OPS="$MIX_BURST_INTERVAL_OPS" \
MIX_BURST_SCAN_NEXTS="$MIX_BURST_SCAN_NEXTS" \
MIX_BURST_COLD_RANGES_ONLY="$MIX_BURST_COLD_RANGES_ONLY" \
MIX_MONITOR_ENABLE="$MIX_MONITOR_ENABLE" \
MIX_MONITOR_WINDOW_US="$MIX_MONITOR_WINDOW_US" \
SIMFS_MONITOR_ENABLE="$SIMFS_MONITOR_ENABLE" \
SIMFS_MONITOR_WINDOW_US="$SIMFS_MONITOR_WINDOW_US" \
SIMFS_MONITOR_STAGE_SECONDS="$SIMFS_MONITOR_STAGE_SECONDS" \
DB_DIR="$DB_DIR" \
WAL_DIR="$WAL_DIR" \
EXTRA_DB_BENCH_ARGS="$EXTRA_DB_BENCH_ARGS" \
  bash "$RUNNER"

echo "[exp34] db size (DB_DIR):"
du -sh "$(dirname "$DB_DIR")" || true

echo "[exp34] post-process figures:"
python3 "$ROOT_DIR/tools/plot_mixgraph_monitoring.py" --run_dir "$OUT_DIR" || true

echo "[exp34] figures under: $OUT_DIR/monitor_figures"
