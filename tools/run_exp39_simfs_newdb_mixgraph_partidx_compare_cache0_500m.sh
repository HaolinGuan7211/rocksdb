#!/usr/bin/env bash
set -euo pipefail

# Exp39: compare partitioned index vs non-partitioned index on the same
# mixgraph (shift+burst) workload under simulated_hybrid_file_system.
#
# Runs two cases sequentially (reusing the same DB_DIR to avoid double space):
#   - nonpart: default BlockBasedTable index (binary search)
#   - partidx: partitioned index (two-level), via --partition_index=1
#
# Two cache tiers:
#   - cache_size=0 (effectively block cache off)
#   - cache_size=500MB
#
# Usage:
#   bash tools/run_exp39_simfs_newdb_mixgraph_partidx_compare_cache0_500m.sh
#
# Common overrides:
#   TARGET_DB_GIB=4 THREADS=8 REALISTIC_READS=200000 \
#     bash tools/run_exp39_simfs_newdb_mixgraph_partidx_compare_cache0_500m.sh
#
# Notes:
# - mixgraph Seek should not miss => uses fillseq.
# - For stable time-series plots, default uses --duration=120s for mixgraph.

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
GLOBAL_EXP_ID="${GLOBAL_EXP_ID:-39}"
EXPERIMENT_NAME="${EXPERIMENT_NAME:-simfs_newdb_mixgraph_partidx_compare_cache0_500m}"
RUN_TIME="${RUN_TIME:-$(date +%H%M%S)}"
EXPERIMENT_DIR="${EXPERIMENT_DIR:-$ROOT_DIR/experiment/${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}}"

# DB shape (default small for iteration; override to 4 for final thesis runs).
TARGET_DB_GIB="${TARGET_DB_GIB:-0.05}"
KEY_SIZE="${KEY_SIZE:-16}"
VALUE_SIZE="${VALUE_SIZE:-1024}"
COMPRESSION_TYPE="${COMPRESSION_TYPE:-lz4}"

# Workload knobs (seek-heavy mixgraph).
THREADS="${THREADS:-2}"
FILL_THREADS="${FILL_THREADS:-$THREADS}"
REALISTIC_READS="${REALISTIC_READS:-200000000}" # per-thread, bounded by duration
MIXGRAPH_DURATION_SECONDS="${MIXGRAPH_DURATION_SECONDS:-120}"

MIX_GET_RATIO="${MIX_GET_RATIO:-0.15}"
MIX_PUT_RATIO="${MIX_PUT_RATIO:-0.00}"
MIX_SEEK_RATIO="${MIX_SEEK_RATIO:-0.75}"
MIX_MULTIGET_RATIO="${MIX_MULTIGET_RATIO:-0.10}"
MIX_MULTIGET_BATCH="${MIX_MULTIGET_BATCH:-16}"

# Locality + shift + burst (keep enabled per requirement).
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
MIX_BURST_INTERVAL_OPS="${MIX_BURST_INTERVAL_OPS:-50000}"
MIX_BURST_SCAN_NEXTS="${MIX_BURST_SCAN_NEXTS:-200}"
MIX_BURST_COLD_RANGES_ONLY="${MIX_BURST_COLD_RANGES_ONLY:-1}"

# Two cache tiers: OFF (0) and 500MB.
CACHE_SIZES="${CACHE_SIZES:-0,536870912}"

# DB paths (reused across cases to avoid double space).
DB_DIR="${DB_DIR:-/tmp/rocksdb_simfs_partidx_compare/db}"
WAL_DIR="${WAL_DIR:-/tmp/rocksdb_simfs_partidx_compare/wal}"

# Optional: redirect simfs-managed files to a tmpfs root to minimize base FS noise.
USE_TMPFS_REDIRECT="${USE_TMPFS_REDIRECT:-0}" # 0|1
TMPFS_ROOT="${TMPFS_ROOT:-/dev/shm/nvm_tmpfs_root.exp39_simfs}"

# Monitoring + histogram for P50/P95/P99 extraction.
MIX_MONITOR_ENABLE="${MIX_MONITOR_ENABLE:-1}"
SIMFS_MONITOR_ENABLE="${SIMFS_MONITOR_ENABLE:-1}"
MIX_MONITOR_WINDOW_US="${MIX_MONITOR_WINDOW_US:-1000000}"
SIMFS_MONITOR_WINDOW_US="${SIMFS_MONITOR_WINDOW_US:-1000000}"
SIMFS_MONITOR_STAGE_SECONDS="${SIMFS_MONITOR_STAGE_SECONDS:-$MIX_SHIFT_STAGE_SECONDS}"

# Which case(s) to run: nonpart | partidx | both
RUN_CASES="${RUN_CASES:-both}"

# Tail probe (slow samples for P99 composition).
TAIL_PROBE_ENABLE="${TAIL_PROBE_ENABLE:-1}"
TAIL_PROBE_THRESHOLD_US="${TAIL_PROBE_THRESHOLD_US:-1000}"
TAIL_PROBE_MAX_SAMPLES="${TAIL_PROBE_MAX_SAMPLES:-20000}"

mkdir -p "$EXPERIMENT_DIR"

NUM_KEYS="$(awk -v gib="$TARGET_DB_GIB" -v k="$KEY_SIZE" -v v="$VALUE_SIZE" \
  'BEGIN { bytes=gib*1024*1024*1024; per=k+v; n=int(bytes/per); if(n<1)n=1; print n }')"

echo "[exp39] EXPERIMENT_DIR=$EXPERIMENT_DIR"
echo "[exp39] target_db_gib=$TARGET_DB_GIB key_size=$KEY_SIZE value_size=$VALUE_SIZE -> num_keys=$NUM_KEYS"
echo "[exp39] cache_sizes=$CACHE_SIZES threads=$THREADS duration=$MIXGRAPH_DURATION_SECONDS"
echo "[exp39] tmpfs_redirect=$USE_TMPFS_REDIRECT tmpfs_root=$TMPFS_ROOT"
echo "[exp39] DB_DIR=$DB_DIR WAL_DIR=$WAL_DIR"

run_case() {
  local case_label="$1"
  shift
  local extra_flags=("$@")

  local run_tag="${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}_${case_label}_${RUN_TIME}"
  local out_dir="$EXPERIMENT_DIR/run_results/$run_tag"
  mkdir -p "$out_dir"

  echo "[exp39][$case_label] OUT_DIR=$out_dir"
  echo "[exp39][$case_label] clearing DB_DIR/WAL_DIR"
  rm -rf "$DB_DIR" "$WAL_DIR"
  if [[ "$USE_TMPFS_REDIRECT" == "1" ]]; then
    echo "[exp39][$case_label] clearing TMPFS_ROOT=$TMPFS_ROOT"
    rm -rf "$TMPFS_ROOT"
  fi
  mkdir -p "$WAL_DIR"

  local extra_args_array=(
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
    extra_args_array+=(
      --simulate_xp_redirect_to_tmpfs=1
      --simulate_xp_tmpfs_root="$TMPFS_ROOT"
    )
  fi
  extra_args_array+=("${extra_flags[@]}")
  local extra_db_bench_args="${EXTRA_DB_BENCH_ARGS:-${extra_args_array[*]}}"

  RUN_TAG="$run_tag" \
  EXPERIMENT_DIR="$EXPERIMENT_DIR" \
  OUT_DIR="$out_dir" \
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
  SKIP_FILL=0 \
  FILL_BENCHMARK=fillseq \
  ISOLATE_BY_CACHE=0 \
  RUN_ONLY_MIXGRAPH=1 \
  MIXGRAPH_DURATION_SECONDS="$MIXGRAPH_DURATION_SECONDS" \
  MIX_GET_RATIO="$MIX_GET_RATIO" \
  MIX_PUT_RATIO="$MIX_PUT_RATIO" \
  MIX_SEEK_RATIO="$MIX_SEEK_RATIO" \
  MIX_MULTIGET_RATIO="$MIX_MULTIGET_RATIO" \
  MIX_MULTIGET_BATCH="$MIX_MULTIGET_BATCH" \
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
  TAIL_PROBE_ENABLE="$TAIL_PROBE_ENABLE" \
  TAIL_PROBE_THRESHOLD_US="$TAIL_PROBE_THRESHOLD_US" \
  TAIL_PROBE_MAX_SAMPLES="$TAIL_PROBE_MAX_SAMPLES" \
  TAIL_PROBE_CASE_LABEL="$case_label" \
  TAIL_PROBE_SCENARIO=mixgraph \
  DB_DIR="$DB_DIR" \
  WAL_DIR="$WAL_DIR" \
  EXTRA_DB_BENCH_ARGS="$extra_db_bench_args" \
    bash "$RUNNER"

  echo "[exp39][$case_label] post-process figures"
  python3 "$ROOT_DIR/tools/plot_mixgraph_monitoring.py" --run_dir "$out_dir" || true
  echo "[exp39][$case_label] figures under: $out_dir/monitor_figures"
}

case "$RUN_CASES" in
  both)
    run_case "nonpart"
    run_case "partidx" --partition_index=1 --metadata_block_size=16384
    ;;
  nonpart)
    run_case "nonpart"
    ;;
  partidx)
    run_case "partidx" --partition_index=1 --metadata_block_size=16384
    ;;
  *)
    echo "[fatal] RUN_CASES must be one of: nonpart|partidx|both (got: $RUN_CASES)" >&2
    exit 1
    ;;
esac

echo "[exp39] done. Results under: $EXPERIMENT_DIR/run_results/"
