#!/usr/bin/env bash
set -euo pipefail

# Exp50: Compare baseline vs SSTHashSeek (experimental per-SST hash index) under
# simulated_hybrid_file_system (SimFS) using mixgraph (seek-heavy) workload.
#
# This runner is intentionally "existing-DB first":
# - It (re)builds a single DB with SSTHashSeek meta blocks enabled
# - Then runs mixgraph on that same DB twice:
#     (1) baseline: --experimental_sst_hash_index_enable=0
#     (2) hashseek: --experimental_sst_hash_index_enable=1
# - For each case, it runs both cache_size=0 and cache_size=500MB.
#
# Typical usage:
#   TARGET_DB_GIB=4 THREADS=8 FILL_THREADS=8 MIXGRAPH_DURATION_SECONDS=180 \
#     bash tools/run_exp50_simfs_mixgraph_sst_hash_seek_compare_cache0_500m.sh

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Prefer the CMake/ninja-built binary (it includes SimFS + probe flags used in
# exp scripts). Allow override via $DB_BENCH.
DB_BENCH="${DB_BENCH:-$ROOT_DIR/build/db_bench}"

if [[ ! -x "$DB_BENCH" ]]; then
  echo "missing db_bench: $DB_BENCH" >&2
  exit 1
fi

EXP_DATE="${EXP_DATE:-$(date +%Y%m%d)}"
GLOBAL_EXP_ID="${GLOBAL_EXP_ID:-50}"
EXPERIMENT_NAME="${EXPERIMENT_NAME:-simfs_mixgraph_sst_hash_seek_compare_cache0_500m}"
RUN_TIME="${RUN_TIME:-$(date +%H%M%S)}"
EXPERIMENT_DIR="${EXPERIMENT_DIR:-$ROOT_DIR/experiment/${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}}"

# DB shape
TARGET_DB_GIB="${TARGET_DB_GIB:-4}"
KEY_SIZE="${KEY_SIZE:-16}"
VALUE_SIZE="${VALUE_SIZE:-1024}"
COMPRESSION_TYPE="${COMPRESSION_TYPE:-lz4}"
COMPRESSION_RATIO="${COMPRESSION_RATIO:-0.10}"

THREADS="${THREADS:-8}"
FILL_THREADS="${FILL_THREADS:-$THREADS}"
REALISTIC_READS="${REALISTIC_READS:-200000000}"
MIXGRAPH_DURATION_SECONDS="${MIXGRAPH_DURATION_SECONDS:-180}"
SEED="${SEED:-12345}"
PERF_LEVEL="${PERF_LEVEL:-1}"

# Latest tuned shift+burst (keep in sync with exp42/exp49).
MIX_GET_RATIO="${MIX_GET_RATIO:-0.15}"
MIX_PUT_RATIO="${MIX_PUT_RATIO:-0.00}"
MIX_SEEK_RATIO="${MIX_SEEK_RATIO:-0.75}"

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

# Probes (enabled by default).
SAMPLE_PROBE_ENABLE="${SAMPLE_PROBE_ENABLE:-1}"
SAMPLE_PROBE_INTERVAL_OPS="${SAMPLE_PROBE_INTERVAL_OPS:-5000}"
SAMPLE_PROBE_MAX_SAMPLES="${SAMPLE_PROBE_MAX_SAMPLES:-20000}"
SAMPLE_PROBE_OP="${SAMPLE_PROBE_OP:-seek}"

TAIL_PROBE_ENABLE="${TAIL_PROBE_ENABLE:-1}"
TAIL_PROBE_THRESHOLD_US="${TAIL_PROBE_THRESHOLD_US:-1000}"
TAIL_PROBE_MAX_SAMPLES="${TAIL_PROBE_MAX_SAMPLES:-20000}"
TAIL_PROBE_OP="${TAIL_PROBE_OP:-seek}"

SIMFS_MONITOR_ENABLE="${SIMFS_MONITOR_ENABLE:-1}"
SIMFS_MONITOR_WINDOW_US="${SIMFS_MONITOR_WINDOW_US:-1000000}"
SIMFS_MONITOR_STAGE_SECONDS="${SIMFS_MONITOR_STAGE_SECONDS:-$MIX_SHIFT_STAGE_SECONDS}"
SIMFS_MONITOR_MAX_READ="${SIMFS_MONITOR_MAX_READ:-1}"
SIMFS_MONITOR_MAX_OPEN="${SIMFS_MONITOR_MAX_OPEN:-1}"
SIMFS_MONITOR_MAX_PREFETCH="${SIMFS_MONITOR_MAX_PREFETCH:-1}"

# SimFS model knobs (fast NVM regime by default).
XP_LATENCY_NS="${XP_LATENCY_NS:-75}"
SIMULATE_XP_BUSY_WAIT="${SIMULATE_XP_BUSY_WAIT:-1}"
SIMULATE_XP_MMAP_BASE_IO="${SIMULATE_XP_MMAP_BASE_IO:-1}"

# Experimental hash index knobs.
SST_HASH_LOAD_FACTOR="${SST_HASH_LOAD_FACTOR:-0.90}"
SST_HASH_FP_BITS="${SST_HASH_FP_BITS:-16}"
SST_HASH_PIN="${SST_HASH_PIN:-1}"

# DB location (single shared DB across baseline/hashseek for fairness).
DB_DIR="${DB_DIR:-/tmp/rocksdb_simfs_hashseek/db}"
WAL_DIR="${WAL_DIR:-/tmp/rocksdb_simfs_hashseek/wal}"

# Benchmark cache sizes: 0 and 500MB.
CACHE_SIZES="${CACHE_SIZES:-0,536870912}"

mkdir -p "$EXPERIMENT_DIR"
echo "$RUN_TIME" >"$EXPERIMENT_DIR/.run_time"

num_keys="$(
  awk -v gib="$TARGET_DB_GIB" -v k="$KEY_SIZE" -v v="$VALUE_SIZE" \
    'BEGIN { bytes=gib*1024*1024*1024; per=k+v; n=int(bytes/per); if(n<1)n=1; print n }'
)"

echo "[exp50] EXPERIMENT_DIR=$EXPERIMENT_DIR"
echo "[exp50] DB_DIR=$DB_DIR WAL_DIR=$WAL_DIR"
echo "[exp50] target_db_gib=$TARGET_DB_GIB key_size=$KEY_SIZE value_size=$VALUE_SIZE -> num_keys=$num_keys"
echo "[exp50] threads=$THREADS fill_threads=$FILL_THREADS duration=$MIXGRAPH_DURATION_SECONDS cache_sizes=$CACHE_SIZES"
echo "[exp50] xp_latency_ns=$XP_LATENCY_NS busy_wait=$SIMULATE_XP_BUSY_WAIT mmap_base_io=$SIMULATE_XP_MMAP_BASE_IO"

safe_rmtree() {
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

db_has_sst_files() {
  local db="$1"
  if [[ ! -d "$db" ]]; then
    return 1
  fi
  # If it has at least one .sst file, treat as "built".
  local n
  n="$(ls -1 "$db"/*.sst 2>/dev/null | wc -l | tr -d ' ')"
  [[ "${n:-0}" != "0" ]]
}

rebuild_db() {
  echo "[exp50] rebuild_db: clearing $DB_DIR $WAL_DIR"
  safe_rmtree "$DB_DIR"
  safe_rmtree "$WAL_DIR"
  mkdir -p "$DB_DIR" "$WAL_DIR"

  echo "[exp50] rebuild_db: fillseq + waitforcompaction (sst_hash_index_enable=1)"
  "$DB_BENCH" \
    --db="$DB_DIR" \
    --wal_dir="$WAL_DIR" \
    --benchmarks=fillseq,waitforcompaction,stats \
    --statistics \
    --num="$num_keys" \
    --key_size="$KEY_SIZE" \
    --value_size="$VALUE_SIZE" \
    --threads="$FILL_THREADS" \
    --compression_type="$COMPRESSION_TYPE" \
    --compression_ratio="$COMPRESSION_RATIO" \
    --cache_size=$((32<<20)) \
    --use_direct_reads=true \
    --use_direct_io_for_flush_and_compaction=true \
    --disable_wal=1 \
    --seed="$SEED" \
    --perf_level="$PERF_LEVEL" \
    --index_with_first_key=1 \
    --index_shortening_mode=0 \
    --block_restart_interval=16 \
    --index_block_restart_interval=1 \
    --simulate_xp_nvm=1 \
    --simulate_xp_levels=0,1,2,3,4,5,6 \
    --simulate_xp_line_bytes=256 \
    --simulate_xp_buffer_bytes=16384 \
    --simulate_xp_latency_ns="$XP_LATENCY_NS" \
    --simulate_xp_rpq_depth=64 \
    --simulate_xp_wpq_depth=64 \
    --simulate_xp_wpq_submit_ns=100 \
    --simulate_xp_prefetch_hit_ns=120 \
    --simulate_xp_enable_prefetch=true \
    --simulate_xp_busy_wait="$SIMULATE_XP_BUSY_WAIT" \
    --simulate_xp_mmap_base_io="$SIMULATE_XP_MMAP_BASE_IO" \
    --experimental_sst_hash_index_enable=1 \
    --experimental_sst_hash_index_pin="$SST_HASH_PIN" \
    --experimental_sst_hash_index_load_factor="$SST_HASH_LOAD_FACTOR" \
    --experimental_sst_hash_index_fingerprint_bits="$SST_HASH_FP_BITS" \
    2>&1 | tee "$EXPERIMENT_DIR/fill_db.log"

  if ! db_has_sst_files "$DB_DIR"; then
    echo "[exp50] ERROR: rebuild_db produced no .sst files under $DB_DIR" >&2
    exit 2
  fi
}

FORCE_REBUILD_DB="${FORCE_REBUILD_DB:-0}"
if [[ "$FORCE_REBUILD_DB" == "1" ]] || ! db_has_sst_files "$DB_DIR"; then
  rebuild_db
else
  echo "[exp50] reuse existing DB (sst files found under $DB_DIR)"
fi

run_case() {
  local label="$1"
  local enable="$2"
  local out_var="$3"

  local run_root="$EXPERIMENT_DIR/run_results/${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}_${label}_${RUN_TIME}"
  mkdir -p "$run_root"
  printf -v "$out_var" '%s' "$run_root"
  echo "[exp50][$label] RUN_ROOT=$run_root (hash_index_enable=$enable)"

  IFS=',' read -r -a _caches <<<"$CACHE_SIZES"
  local cache
  for cache in "${_caches[@]}"; do
    cache="$(echo "$cache" | tr -d ' ')"
    local out_dir="$run_root/cache_${cache}"
    mkdir -p "$out_dir"

    local sample_csv="$out_dir/sample_probe.csv"
    local tail_csv="$out_dir/tail_probe.csv"
    local simfs_window_csv="$out_dir/simfs_window.csv"
    local simfs_stage_csv="$out_dir/simfs_stage.csv"
    local simfs_stats_txt="$out_dir/simfs_stats.txt"

    echo "[exp50][$label][cache=$cache] OUT_DIR=$out_dir"
    {
      echo "$DB_BENCH \\"
      echo "  --db=\"$DB_DIR\" --wal_dir=\"$WAL_DIR\" --use_existing_db=1 \\"
      echo "  --benchmarks=mixgraph,stats --statistics --histogram=1 --duration=\"$MIXGRAPH_DURATION_SECONDS\" \\"
      echo "  --num=\"$num_keys\" --reads=\"$REALISTIC_READS\" --threads=\"$THREADS\" --key_size=\"$KEY_SIZE\" \\"
      echo "  --compression_type=\"$COMPRESSION_TYPE\" --cache_size=\"$cache\" \\"
      echo "  --readonly=1 --disable_auto_compactions=1 --disable_wal=1 \\"
      echo "  --seed=\"$SEED\" --perf_level=\"$PERF_LEVEL\" \\"
      echo "  --use_direct_reads=true --use_direct_io_for_flush_and_compaction=true \\"
      echo "  --mix_get_ratio=\"$MIX_GET_RATIO\" --mix_put_ratio=\"$MIX_PUT_RATIO\" --mix_seek_ratio=\"$MIX_SEEK_RATIO\" \\"
      echo "  --key_dist_a=\"$MIX_KEY_DIST_A\" --key_dist_b=\"$MIX_KEY_DIST_B\" \\"
      echo "  --keyrange_dist_a=\"$MIX_KEYRANGE_DIST_A\" --keyrange_dist_b=\"$MIX_KEYRANGE_DIST_B\" \\"
      echo "  --keyrange_dist_c=\"$MIX_KEYRANGE_DIST_C\" --keyrange_dist_d=\"$MIX_KEYRANGE_DIST_D\" \\"
      echo "  --keyrange_num=\"$MIX_KEYRANGE_NUM\" \\"
      echo "  --mix_hot_keyrange_count=0 --mix_hotset_enable=\"$MIX_HOTSET_ENABLE\" \\"
      echo "  --mix_hotset_range_pct=\"$MIX_HOTSET_RANGE_PCT\" --mix_hotset_range_access_pct=\"$MIX_HOTSET_RANGE_ACCESS_PCT\" \\"
      echo "  --mix_hotset_range_zipf_theta=\"$MIX_HOTSET_RANGE_ZIPF_THETA\" --mix_hotset_key_pct=\"$MIX_HOTSET_KEY_PCT\" \\"
      echo "  --mix_hotset_key_access_pct=\"$MIX_HOTSET_KEY_ACCESS_PCT\" --mix_hotset_evenly_spread_ranges=\"$MIX_HOTSET_EVENLY_SPREAD_RANGES\" \\"
      echo "  --mix_shift_enable=\"$MIX_SHIFT_ENABLE\" --mix_shift_mode=\"$MIX_SHIFT_MODE\" --mix_shift_stage_seconds=\"$MIX_SHIFT_STAGE_SECONDS\" \\"
      echo "  --mix_shift_stride_ranges=\"$MIX_SHIFT_STRIDE_RANGES\" --mix_shift_jump_multiplier=\"$MIX_SHIFT_JUMP_MULTIPLIER\" \\"
      echo "  --mix_shift_base_start_range=\"$MIX_SHIFT_BASE_START_RANGE\" \\"
      echo "  --iter_k=\"$MIX_ITER_K\" --iter_sigma=\"$MIX_ITER_SIGMA\" --iter_theta=\"$MIX_ITER_THETA\" \\"
      echo "  --sample_probe_output=\"$sample_csv\" --sample_probe_interval_ops=\"$SAMPLE_PROBE_INTERVAL_OPS\" \\"
      echo "  --sample_probe_max_samples=\"$SAMPLE_PROBE_MAX_SAMPLES\" --sample_probe_op=\"$SAMPLE_PROBE_OP\" \\"
      echo "  --sample_probe_case_label=\"$label\" --sample_probe_scenario=\"mixgraph\" \\"
      echo "  --tail_probe_output=\"$tail_csv\" --tail_probe_threshold_us=\"$TAIL_PROBE_THRESHOLD_US\" \\"
      echo "  --tail_probe_max_samples=\"$TAIL_PROBE_MAX_SAMPLES\" --tail_probe_op=\"$TAIL_PROBE_OP\" \\"
      echo "  --tail_probe_case_label=\"$label\" --tail_probe_scenario=\"mixgraph\" \\"
      echo "  --simulate_xp_stats_file=\"$simfs_stats_txt\" \\"
      echo "  --simulate_xp_monitor_enable=\"$SIMFS_MONITOR_ENABLE\" --simulate_xp_monitor_window_us=\"$SIMFS_MONITOR_WINDOW_US\" \\"
      echo "  --simulate_xp_monitor_stage_seconds=\"$SIMFS_MONITOR_STAGE_SECONDS\" --simulate_xp_monitor_max_read=\"$SIMFS_MONITOR_MAX_READ\" \\"
      echo "  --simulate_xp_monitor_max_open=\"$SIMFS_MONITOR_MAX_OPEN\" --simulate_xp_monitor_max_prefetch=\"$SIMFS_MONITOR_MAX_PREFETCH\" \\"
      echo "  --simulate_xp_monitor_window_csv=\"$simfs_window_csv\" --simulate_xp_monitor_stage_csv=\"$simfs_stage_csv\" \\"
      echo "  --simulate_xp_nvm=1 --simulate_xp_levels=0,1,2,3,4,5,6 --simulate_xp_latency_ns=\"$XP_LATENCY_NS\" \\"
      echo "  --simulate_xp_line_bytes=256 --simulate_xp_buffer_bytes=16384 --simulate_xp_rpq_depth=64 --simulate_xp_wpq_depth=64 \\"
      echo "  --simulate_xp_wpq_submit_ns=100 --simulate_xp_prefetch_hit_ns=120 --simulate_xp_enable_prefetch=true \\"
      echo "  --simulate_xp_busy_wait=\"$SIMULATE_XP_BUSY_WAIT\" --simulate_xp_mmap_base_io=\"$SIMULATE_XP_MMAP_BASE_IO\" \\"
      echo "  --index_with_first_key=1 --index_shortening_mode=0 --block_restart_interval=16 --index_block_restart_interval=1 \\"
      echo "  --experimental_sst_hash_index_enable=\"$enable\" --experimental_sst_hash_index_pin=\"$SST_HASH_PIN\""
    } >"$out_dir/mixgraph.cmd"

    if [[ "$SAMPLE_PROBE_ENABLE" != "1" ]]; then
      rm -f "$sample_csv"
      sample_csv=""
    fi
    if [[ "$TAIL_PROBE_ENABLE" != "1" ]]; then
      rm -f "$tail_csv"
      tail_csv=""
    fi

    # Run.
    bash "$out_dir/mixgraph.cmd" 2>&1 | tee "$out_dir/mixgraph.log"

    # Post-process probes (if present).
    if [[ -n "${sample_csv:-}" && -f "$sample_csv" ]]; then
      mkdir -p "$out_dir/sample_probe_${SAMPLE_PROBE_OP}"
      python3 "$ROOT_DIR/tools/analyze_tail_probe_samples.py" \
        --samples_csv "$sample_csv" \
        --out_dir "$out_dir/sample_probe_${SAMPLE_PROBE_OP}" \
        --label "$label" \
        --phase "mixgraph_cache_${cache}" \
        --scenario "mixgraph" \
        --kind sample \
        --threshold_us 0 \
        >"$out_dir/sample_probe_${SAMPLE_PROBE_OP}/analyze.log" 2>&1
    fi
    if [[ -n "${tail_csv:-}" && -f "$tail_csv" ]]; then
      mkdir -p "$out_dir/tail_probe_${TAIL_PROBE_OP}"
      python3 "$ROOT_DIR/tools/analyze_tail_probe_samples.py" \
        --samples_csv "$tail_csv" \
        --out_dir "$out_dir/tail_probe_${TAIL_PROBE_OP}" \
        --label "$label" \
        --phase "mixgraph_cache_${cache}" \
        --scenario "mixgraph" \
        --kind tail \
        --threshold_us "$TAIL_PROBE_THRESHOLD_US" \
        >"$out_dir/tail_probe_${TAIL_PROBE_OP}/analyze.log" 2>&1
    fi
  done
}

baseline_dir=""
hashseek_dir=""
run_case baseline 0 baseline_dir
run_case sst_hash_seek 1 hashseek_dir

echo "[exp50] baseline_dir=$baseline_dir"
echo "[exp50] hashseek_dir=$hashseek_dir"

compare_dir="$EXPERIMENT_DIR/compare_report"
mkdir -p "$compare_dir"
python3 "$ROOT_DIR/tools/report_exp50_sst_hash_seek_compare.py" \
  --baseline_dir "$baseline_dir" \
  --hashseek_dir "$hashseek_dir" \
  --out_dir "$compare_dir" \
  --cache_sizes "$CACHE_SIZES" \
  | tee "$compare_dir/report.log"

echo "[exp50] DONE: $compare_dir/report.md"
