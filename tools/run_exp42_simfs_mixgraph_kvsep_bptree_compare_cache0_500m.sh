#!/usr/bin/env bash
set -euo pipefail

# Exp42: KV-separation + B+Tree SST (experimental) under simulated_hybrid_file_system
# with mixgraph (shift+burst), comparing baseline vs kvsep.
#
# NOTE: On this branch we first add flags + plan. The kvsep SST implementation
# will be brought up incrementally; this script is the intended runner once the
# format is wired into table builder/reader.
#
# Typical overrides:
#   TARGET_DB_GIB=4 THREADS=8 FILL_THREADS=8 MIXGRAPH_DURATION_SECONDS=600 \
#   USE_TMPFS_REDIRECT=1 TMPFS_ROOT=/dev/shm/nvm_tmpfs_root.exp42_simfs \
#     bash tools/run_exp42_simfs_mixgraph_kvsep_bptree_compare_cache0_500m.sh

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
GLOBAL_EXP_ID="${GLOBAL_EXP_ID:-42}"
EXPERIMENT_NAME="${EXPERIMENT_NAME:-simfs_mixgraph_kvsep_bptree_compare_cache0_500m}"
RUN_TIME="${RUN_TIME:-$(date +%H%M%S)}"
EXPERIMENT_DIR="${EXPERIMENT_DIR:-$ROOT_DIR/experiment/${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}}"

# DB shape (override to 4 for full runs; default small for iteration)
TARGET_DB_GIB="${TARGET_DB_GIB:-0.10}"
KEY_SIZE="${KEY_SIZE:-16}"
VALUE_SIZE="${VALUE_SIZE:-1024}"
COMPRESSION_TYPE="${COMPRESSION_TYPE:-lz4}"

THREADS="${THREADS:-4}"
FILL_THREADS="${FILL_THREADS:-$THREADS}"
REALISTIC_READS="${REALISTIC_READS:-200000000}"
MIXGRAPH_DURATION_SECONDS="${MIXGRAPH_DURATION_SECONDS:-180}"
SEED="${SEED:-12345}"
PERF_LEVEL="${PERF_LEVEL:-1}"

# Latest tuned shift+burst (keep in sync with other exp scripts).
MIX_GET_RATIO="${MIX_GET_RATIO:-0.15}"
MIX_PUT_RATIO="${MIX_PUT_RATIO:-0.00}"
MIX_SEEK_RATIO="${MIX_SEEK_RATIO:-0.75}"
MIX_MULTIGET_RATIO="${MIX_MULTIGET_RATIO:-0.10}"
MIX_MULTIGET_BATCH="${MIX_MULTIGET_BATCH:-16}"

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

# Monitoring / probes (enabled by default for this experiment).
MIX_MONITOR_ENABLE="${MIX_MONITOR_ENABLE:-1}"
MIX_MONITOR_WINDOW_US="${MIX_MONITOR_WINDOW_US:-1000000}"
MIX_PROBE_ENABLE="${MIX_PROBE_ENABLE:-1}"
MIX_PROBE_INTERVAL_OPS="${MIX_PROBE_INTERVAL_OPS:-50000}"
MIX_PROBE_READS="${MIX_PROBE_READS:-64}"

SIMFS_MONITOR_ENABLE="${SIMFS_MONITOR_ENABLE:-1}"
SIMFS_MONITOR_WINDOW_US="${SIMFS_MONITOR_WINDOW_US:-1000000}"
# Align simfs stage buckets with shift stage by default.
SIMFS_MONITOR_STAGE_SECONDS="${SIMFS_MONITOR_STAGE_SECONDS:-$MIX_SHIFT_STAGE_SECONDS}"
SIMFS_MONITOR_MAX_READ="${SIMFS_MONITOR_MAX_READ:-1}"
SIMFS_MONITOR_MAX_OPEN="${SIMFS_MONITOR_MAX_OPEN:-1}"
SIMFS_MONITOR_MAX_PREFETCH="${SIMFS_MONITOR_MAX_PREFETCH:-1}"

# Post-run tail probe rerun + attribution (P99 composition).
POST_TAIL_PROBE_ENABLE="${POST_TAIL_PROBE_ENABLE:-1}"
TAIL_PROBE_MAX_SAMPLES="${TAIL_PROBE_MAX_SAMPLES:-20000}"
POST_TAIL_PROBE_DURATION_SECONDS="${POST_TAIL_PROBE_DURATION_SECONDS:-180}"

# Which cases to run: "baseline,kvsep_bptree" (default) or a subset.
RUN_CASES="${RUN_CASES:-baseline,kvsep_bptree}"

# Compare cache=0 and cache=500MB by default.
CACHE_SIZES="${CACHE_SIZES:-0,536870912}"

DB_DIR="${DB_DIR:-/tmp/rocksdb_simfs_kvsep_bptree/db}"
WAL_DIR="${WAL_DIR:-/tmp/rocksdb_simfs_kvsep_bptree/wal}"

USE_TMPFS_REDIRECT="${USE_TMPFS_REDIRECT:-1}"
TMPFS_ROOT="${TMPFS_ROOT:-/dev/shm/nvm_tmpfs_root.exp42_simfs}"

SUPER_BLOCK_BYTES="${SUPER_BLOCK_BYTES:-16384}"

# KV-SEP knobs (format construction-time).
KVSEP_ENABLE="${KVSEP_ENABLE:-1}"
KVSEP_LEAF_BYTES="${KVSEP_LEAF_BYTES:-16384}"
KVSEP_VALUE_BYTES="${KVSEP_VALUE_BYTES:-16384}"
KVSEP_FANOUT="${KVSEP_FANOUT:-64}"

mkdir -p "$EXPERIMENT_DIR"

NUM_KEYS="$(awk -v gib="$TARGET_DB_GIB" -v k="$KEY_SIZE" -v v="$VALUE_SIZE" \
  'BEGIN { bytes=gib*1024*1024*1024; per=k+v; n=int(bytes/per); if(n<1)n=1; print n }')"

echo "[exp42] EXPERIMENT_DIR=$EXPERIMENT_DIR"
echo "[exp42] target_db_gib=$TARGET_DB_GIB key_size=$KEY_SIZE value_size=$VALUE_SIZE -> num_keys=$NUM_KEYS"
echo "[exp42] cache_sizes=$CACHE_SIZES threads=$THREADS duration=$MIXGRAPH_DURATION_SECONDS"
echo "[exp42] tmpfs_redirect=$USE_TMPFS_REDIRECT tmpfs_root=$TMPFS_ROOT"
echo "[exp42] DB_DIR=$DB_DIR WAL_DIR=$WAL_DIR"

common_simfs_args=(
  --histogram=1
  --disable_wal=1
  --seed="$SEED"
  --perf_level="$PERF_LEVEL"
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
  common_simfs_args+=(
    --simulate_xp_redirect_to_tmpfs=1
    --simulate_xp_tmpfs_root="$TMPFS_ROOT"
  )
fi

common_readpath_args=(
  --index_with_first_key=1
  --index_shortening_mode=0
  --super_block_alignment_size="$SUPER_BLOCK_BYTES"
  # Validation requires 0 or >=4. Lower means "too much padding allowed".
  --super_block_alignment_space_overhead_ratio=4
  --enable_super_block_read_coalescing=1
)

run_one() {
  local case_label="$1"
  shift
  local extra_flags=("$@")

  local run_tag="${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}_${case_label}_${RUN_TIME}"
  local out_dir="$EXPERIMENT_DIR/run_results/$run_tag"
  mkdir -p "$out_dir"

  echo "[exp42][$case_label] OUT_DIR=$out_dir"
  rm -rf "$DB_DIR" "$WAL_DIR"
  mkdir -p "$WAL_DIR"
  if [[ "$USE_TMPFS_REDIRECT" == "1" ]]; then
    rm -rf "$TMPFS_ROOT"
  fi

  local base_db_bench_args="${common_simfs_args[*]} ${common_readpath_args[*]} ${extra_flags[*]}"
  # Allow callers to append extra db_bench flags without clobbering the required
  # simfs/readpath flags. For rare cases where you truly want to override all
  # defaults, set EXTRA_DB_BENCH_ARGS_MODE=override.
  local extra_db_bench_args_mode="${EXTRA_DB_BENCH_ARGS_MODE:-append}"
  local user_db_bench_args="${EXTRA_DB_BENCH_ARGS:-}"
  if [[ "$extra_db_bench_args_mode" == "override" ]]; then
    if [[ -z "$user_db_bench_args" ]]; then
      extra_db_bench_args="$base_db_bench_args"
    else
      extra_db_bench_args="$user_db_bench_args"
    fi
  else
    if [[ -z "$user_db_bench_args" ]]; then
      extra_db_bench_args="$base_db_bench_args"
    else
      extra_db_bench_args="$base_db_bench_args $user_db_bench_args"
    fi
  fi

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
  TAIL_PROBE_ENABLE=0 \
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
  EXTRA_DB_BENCH_ARGS="$extra_db_bench_args" \
  DB_DIR="$DB_DIR" \
  WAL_DIR="$WAL_DIR" \
    bash "$RUNNER"

  echo "[exp42][$case_label] post-process: mixgraph monitoring figures"
  python3 "$ROOT_DIR/tools/plot_mixgraph_monitoring.py" --run_dir "$out_dir"
  if [[ "$POST_TAIL_PROBE_ENABLE" == "1" ]]; then
    echo "[exp42][$case_label] post-process: tail probe attribution (max_samples=$TAIL_PROBE_MAX_SAMPLES)"
    python3 "$ROOT_DIR/tools/run_tail_probe_from_run_dir.py" \
      --run_dir "$out_dir" \
      --out_subdir tail_probe \
      --max_samples "$TAIL_PROBE_MAX_SAMPLES" \
      --duration_override_seconds "$POST_TAIL_PROBE_DURATION_SECONDS"
  fi
}

case_wanted() {
  local name="$1"
  IFS=',' read -r -a _cases <<<"$RUN_CASES"
  local c
  for c in "${_cases[@]}"; do
    if [[ "$c" == "$name" ]]; then
      return 0
    fi
  done
  return 1
}

if case_wanted "baseline"; then
  run_one "baseline" \
    --experimental_kvsep_bptree_enable=0
fi

if case_wanted "kvsep_bptree"; then
  run_one "kvsep_bptree" \
    --experimental_kvsep_bptree_enable="$KVSEP_ENABLE" \
    --experimental_kvsep_bptree_leaf_block_bytes="$KVSEP_LEAF_BYTES" \
    --experimental_kvsep_bptree_value_block_bytes="$KVSEP_VALUE_BYTES" \
    --experimental_kvsep_bptree_fanout="$KVSEP_FANOUT"
fi

echo "[exp42] done: $EXPERIMENT_DIR"
