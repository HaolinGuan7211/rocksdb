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
COMPRESSION_RATIO="${COMPRESSION_RATIO:-0.10}"

# Optional CPU-heavy key generator. Useful for amplifying CPU-side costs (e.g.,
# comparator / iterator search) under fast storage models.
CPU_HEAVY_KEYGEN="${CPU_HEAVY_KEYGEN:-0}"
CPU_HEAVY_KEYGEN_FILL_BYTE="${CPU_HEAVY_KEYGEN_FILL_BYTE:-0}"

# Bloom/filter knobs (disabled by default to preserve prior experiment behavior).
# Set BLOOM_BITS=10 (or similar) to enable bloom filters for paper-style
# filter-check attribution experiments.
BLOOM_BITS="${BLOOM_BITS:--1}"

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
# Which ops to attribute in post tail-probe reruns.
# - seek: original tail seek breakdown
# - read: Get/MultiGet tail breakdown (useful for bloom/filter CPU attribution)
POST_TAIL_PROBE_OPS="${POST_TAIL_PROBE_OPS:-seek,read}"
POST_TAIL_PROBE_MIN_THRESHOLD_US="${POST_TAIL_PROBE_MIN_THRESHOLD_US:-2000}"
POST_TAIL_PROBE_P99_MULTIPLIER="${POST_TAIL_PROBE_P99_MULTIPLIER:-1.0}"

# Which cases to run: "baseline,kvsep_bptree" (default) or a subset.
RUN_CASES="${RUN_CASES:-baseline,kvsep_bptree}"

# Compare cache=0 and cache=500MB by default.
CACHE_SIZES="${CACHE_SIZES:-0,536870912}"

DB_DIR="${DB_DIR:-/tmp/rocksdb_simfs_kvsep_bptree/db}"
WAL_DIR="${WAL_DIR:-/tmp/rocksdb_simfs_kvsep_bptree/wal}"

USE_TMPFS_REDIRECT="${USE_TMPFS_REDIRECT:-1}"
TMPFS_ROOT="${TMPFS_ROOT:-/dev/shm/nvm_tmpfs_root.exp42_simfs}"

SUPER_BLOCK_BYTES="${SUPER_BLOCK_BYTES:-16384}"

# SimFS behavior knobs.
XP_LATENCY_NS="${XP_LATENCY_NS:-75}"
SIMULATE_XP_MMAP_BASE_IO="${SIMULATE_XP_MMAP_BASE_IO:-1}"

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
echo "[exp42] compression_ratio=$COMPRESSION_RATIO xp_latency_ns=$XP_LATENCY_NS mmap_base_io=$SIMULATE_XP_MMAP_BASE_IO"

if [[ "$USE_TMPFS_REDIRECT" == "1" ]]; then
  tmpfs_parent="$(dirname "$TMPFS_ROOT")"
  if [[ ! -d "$tmpfs_parent" ]]; then
    echo "[exp42] ERROR: TMPFS_ROOT parent dir missing: $tmpfs_parent" >&2
    exit 2
  fi
  fs_type="$(stat -f -c %T "$tmpfs_parent" 2>/dev/null || echo unknown)"
  if [[ "$fs_type" != "tmpfs" ]]; then
    echo "[exp42] WARN: TMPFS_ROOT parent is not tmpfs (fs_type=$fs_type): $tmpfs_parent" >&2
    echo "[exp42]       This may re-introduce base FS noise into simfs results." >&2
  fi
  avail_bytes="$(df -B1 --output=avail "$tmpfs_parent" | tail -n 1 | tr -d ' ')"
  req_bytes="$(awk -v gib="$TARGET_DB_GIB" -v k="$KEY_SIZE" -v v="$VALUE_SIZE" -v cr="$COMPRESSION_RATIO" \
    'BEGIN { logical=gib*1024*1024*1024; per=(k+v); est=(k+v*cr)/per; printf "%.0f", logical*est*1.25 }')"
  if [[ -n "$avail_bytes" && "$avail_bytes" -lt "$req_bytes" ]]; then
    echo "[exp42] ERROR: insufficient tmpfs space under $tmpfs_parent" >&2
    echo "[exp42]        avail_bytes=$avail_bytes req_bytes~=$req_bytes (target_db_gib=$TARGET_DB_GIB)" >&2
    echo "[exp42]        Suggest: mount a larger tmpfs and set TMPFS_ROOT accordingly." >&2
    exit 2
  fi
fi

common_simfs_args=(
  --histogram=1
  --disable_wal=1
  --seed="$SEED"
  --perf_level="$PERF_LEVEL"
  --simulate_xp_nvm=1
  --simulate_xp_levels=0,1,2,3,4,5,6
  --simulate_xp_line_bytes=256
  --simulate_xp_buffer_bytes=16384
  --simulate_xp_latency_ns="$XP_LATENCY_NS"
  --simulate_xp_rpq_depth=64
  --simulate_xp_wpq_depth=64
  --simulate_xp_wpq_submit_ns=100
  --simulate_xp_prefetch_hit_ns=120
  --simulate_xp_enable_prefetch=true
  # IMPORTANT: For microsecond-/nanosecond-scale delay injection, OS sleep can
  # overshoot by milliseconds under virtualization, causing large tail spikes
  # in simfs_max_latency_per_window. Default to busy-wait to keep experiments
  # stable/reproducible; override via SIMULATE_XP_BUSY_WAIT=0 if needed.
  --simulate_xp_busy_wait="${SIMULATE_XP_BUSY_WAIT:-1}"
  --simulate_xp_mmap_base_io="$SIMULATE_XP_MMAP_BASE_IO"
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

cpu_heavy_keygen_args=()
if [[ "$CPU_HEAVY_KEYGEN" == "1" ]]; then
  cpu_heavy_keygen_args+=(
    --cpu_heavy_keygen=1
    --cpu_heavy_keygen_fill_byte="$CPU_HEAVY_KEYGEN_FILL_BYTE"
  )
fi

filter_args=()
if [[ "$BLOOM_BITS" != "-1" ]]; then
  filter_args+=(--bloom_bits="$BLOOM_BITS")
fi

safe_rmtree() {
  # Avoid relying on `rm -rf` (some environments restrict it). Best-effort.
  local p="$1"
  if [[ -z "$p" ]]; then
    return 0
  fi
  python3 - "$p" <<'PY'
import shutil
import sys
from pathlib import Path
p = Path(sys.argv[1])
try:
    shutil.rmtree(p, ignore_errors=True)
except Exception:
    pass
PY
}

run_one() {
  local case_label="$1"
  shift
  local extra_flags=("$@")

  local run_tag="${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}_${case_label}_${RUN_TIME}"
  local out_dir="$EXPERIMENT_DIR/run_results/$run_tag"
  mkdir -p "$out_dir"

  echo "[exp42][$case_label] OUT_DIR=$out_dir"
  safe_rmtree "$DB_DIR"
  safe_rmtree "$WAL_DIR"
  mkdir -p "$WAL_DIR"
  if [[ "$USE_TMPFS_REDIRECT" == "1" ]]; then
    safe_rmtree "$TMPFS_ROOT"
  fi

  local base_db_bench_args="${common_simfs_args[*]} --compression_ratio=$COMPRESSION_RATIO ${common_readpath_args[*]} ${filter_args[*]} ${cpu_heavy_keygen_args[*]} ${extra_flags[*]}"
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
    # Tail-probe reruns can fail intermittently (e.g., checksum mismatch surfaced
    # as corruption). Retry a small number of times so the overall experiment
    # can proceed and still capture useful samples most of the time.
    local tail_probe_retries="${POST_TAIL_PROBE_RETRIES:-2}"
    local attempt=1
    IFS=',' read -r -a _ops <<<"$POST_TAIL_PROBE_OPS"
    local op
    for op in "${_ops[@]}"; do
      op="$(echo "$op" | tr -d ' ')"
      if [[ -z "$op" ]]; then
        continue
      fi
      local out_subdir="tail_probe_${op}"
      local attempt=1
      while true; do
        if python3 "$ROOT_DIR/tools/run_tail_probe_from_run_dir.py" \
          --run_dir "$out_dir" \
          --out_subdir "$out_subdir" \
          --op "$op" \
          --max_samples "$TAIL_PROBE_MAX_SAMPLES" \
          --min_threshold_us "$POST_TAIL_PROBE_MIN_THRESHOLD_US" \
          --p99_multiplier "$POST_TAIL_PROBE_P99_MULTIPLIER" \
          --duration_override_seconds "$POST_TAIL_PROBE_DURATION_SECONDS"; then
          break
        fi
        if [[ "$attempt" -ge "$tail_probe_retries" ]]; then
          echo "[exp42][$case_label] WARN: tail probe (${op}) failed after ${attempt}/${tail_probe_retries} attempts; continuing." >&2
          break
        fi
        attempt=$((attempt + 1))
        echo "[exp42][$case_label] WARN: tail probe (${op}) failed; retrying attempt ${attempt}/${tail_probe_retries}..." >&2
        sleep 2
      done
    done
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
