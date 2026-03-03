#!/usr/bin/env bash
set -euo pipefail

# Exp47: Gate2 "bypass base I/O" verification sweep.
#
# Purpose:
#   Reuse an existing DB (typically from exp46) and sweep xp_latency_ns with
#   simulate_xp_bypass_base_io=1 to quantify how much of Gate2's base floor is
#   due to real FS/OS path versus simulated device timing.
#
# Default assumes exp46's DB/tmpfs roots are still present.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB_BENCH="${DB_BENCH:-$ROOT_DIR/build/db_bench}"

EXP_DATE="${EXP_DATE:-$(date +%Y%m%d)}"
GLOBAL_EXP_ID="${GLOBAL_EXP_ID:-47}"
EXPERIMENT_NAME="${EXPERIMENT_NAME:-gate2_bypass_base_io_sweep}"
RUN_TIME="${RUN_TIME:-$(date +%H%M%S)}"
EXPERIMENT_DIR="${EXPERIMENT_DIR:-$ROOT_DIR/experiment/${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}_${RUN_TIME}}"

DB_DIR="${DB_DIR:-/tmp/rocksdb_exp46_gate3_cal/db}"
WAL_DIR="${WAL_DIR:-/tmp/rocksdb_exp46_gate3_cal/wal}"

# SimFS.
TMPFS_ROOT="${TMPFS_ROOT:-/dev/shm/nvm_tmpfs_root.exp46_gate3_cal}"
SIMULATE_XP_BUSY_WAIT="${SIMULATE_XP_BUSY_WAIT:-0}"

# Workload + sweep.
NUM_KEYS="${NUM_KEYS:-4129776}"
XP_LATENCY_LIST_NS="${XP_LATENCY_LIST_NS:-75,750,7500,75000}"
THREADS="${THREADS:-1}"
READS="${READS:-5000}"
SAMPLE_INTERVAL_OPS="${SAMPLE_INTERVAL_OPS:-10}"
SAMPLE_MAX_SAMPLES="${SAMPLE_MAX_SAMPLES:-20000}"

# Cache + Bloom.
CACHE_SIZE="${CACHE_SIZE:-536870912}" # 500MB
BLOOM_BITS="${BLOOM_BITS:-2}"
PERF_LEVEL="${PERF_LEVEL:-4}"

# DB shape (must match the existing DB to keep the LSM state comparable).
KEY_SIZE="${KEY_SIZE:-16}"
VALUE_SIZE="${VALUE_SIZE:-1024}"
COMPRESSION_TYPE="${COMPRESSION_TYPE:-lz4}"
COMPRESSION_RATIO="${COMPRESSION_RATIO:-0.10}"
WRITE_BUFFER_SIZE="${WRITE_BUFFER_SIZE:-134217728}"
TARGET_FILE_SIZE_BASE="${TARGET_FILE_SIZE_BASE:-134217728}"
DISABLE_AUTO_COMPACTIONS="${DISABLE_AUTO_COMPACTIONS:-1}"

mkdir -p "$EXPERIMENT_DIR/run_results"

common_simfs_args=(
  --histogram=1
  --disable_wal=1
  --perf_level="$PERF_LEVEL"
  --simulate_xp_nvm=1
  --simulate_xp_levels=0,1,2,3,4,5,6
  --simulate_xp_line_bytes=256
  --simulate_xp_buffer_bytes=16384
  --simulate_xp_rpq_depth=64
  --simulate_xp_wpq_depth=64
  --simulate_xp_wpq_submit_ns=100
  --simulate_xp_prefetch_hit_ns=120
  --simulate_xp_enable_prefetch=true
  --simulate_xp_busy_wait="$SIMULATE_XP_BUSY_WAIT"
  --simulate_xp_redirect_to_tmpfs=1
  --simulate_xp_tmpfs_root="$TMPFS_ROOT"
  --simulate_xp_bypass_base_io=1
)

db_shape_args=(
  --key_size="$KEY_SIZE"
  --value_size="$VALUE_SIZE"
  --compression_type="$COMPRESSION_TYPE"
  --compression_ratio="$COMPRESSION_RATIO"
  --write_buffer_size="$WRITE_BUFFER_SIZE"
  --max_write_buffer_number=4
  --min_write_buffer_number_to_merge=1
  --target_file_size_base="$TARGET_FILE_SIZE_BASE"
  --max_bytes_for_level_base=268435456
  --level0_file_num_compaction_trigger=8
  --max_background_compactions=4
  --bloom_bits=10
)
if [[ "$DISABLE_AUTO_COMPACTIONS" == "1" ]]; then
  db_shape_args+=(--disable_auto_compactions=1)
fi

cache_args=(
  --cache_size="$CACHE_SIZE"
  --cache_index_and_filter_blocks=1
  --pin_top_level_index_and_filter=1
)

points_csv="$EXPERIMENT_DIR/sweep_points.csv"
echo "run_tag,xp_latency_ns,bloom_bits,run_dir" >"$points_csv"

IFS=',' read -r -a XP_ARRAY <<<"$XP_LATENCY_LIST_NS"
for xp_ns in "${XP_ARRAY[@]}"; do
  xp_ns="$(echo "$xp_ns" | tr -d ' ')"
  run_tag="${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}_xplat${xp_ns}_${RUN_TIME}"
  out_dir="$EXPERIMENT_DIR/run_results/$run_tag"
  mkdir -p "$out_dir"

  sample_csv="$out_dir/02_readmissing_cache_${CACHE_SIZE}.sample_probe.csv"
  analysis_dir="$out_dir/analysis_readmissing_sample_cache_${CACHE_SIZE}"

  read_cmd=(
    "$DB_BENCH"
    --db="$DB_DIR"
    --wal_dir="$WAL_DIR"
    --use_existing_db=1
    --benchmarks=readmissing,stats
    --statistics
    --num="$NUM_KEYS"
    --threads="$THREADS"
    --reads="$READS"
    --bloom_bits="$BLOOM_BITS"
    "${common_simfs_args[@]}"
    --simulate_xp_latency_ns="$xp_ns"
    "${db_shape_args[@]}"
    "${cache_args[@]}"
    --sample_probe_output="$sample_csv"
    --sample_probe_op=read
    --sample_probe_interval_ops="$SAMPLE_INTERVAL_OPS"
    --sample_probe_max_samples="$SAMPLE_MAX_SAMPLES"
    --sample_probe_case_label="cache${CACHE_SIZE}"
    --sample_probe_scenario="readmissing_sample"
  )

  printf "%q " "${read_cmd[@]}" >"$out_dir/02_readmissing_cache_${CACHE_SIZE}.cmd"
  printf "\n" >>"$out_dir/02_readmissing_cache_${CACHE_SIZE}.cmd"
  "${read_cmd[@]}" 2>&1 | tee "$out_dir/02_readmissing_cache_${CACHE_SIZE}.log"

  python3 "$ROOT_DIR/tools/analyze_tail_probe_samples.py" \
    --samples_csv "$sample_csv" \
    --out_dir "$analysis_dir" \
    --label "cache${CACHE_SIZE}" \
    --phase "xplat${xp_ns}_bypass1" \
    --scenario "readmissing_sample" \
    --threshold_us 0 \
    --kind sample >/dev/null

  echo "$run_tag,$xp_ns,$BLOOM_BITS,$out_dir" >>"$points_csv"
done

python3 "$ROOT_DIR/tools/summarize_exp44_media_sweep.py" \
  --sweep_points_csv "$points_csv" \
  --out_csv "$EXPERIMENT_DIR/sweep_summary.csv" >/dev/null

python3 "$ROOT_DIR/tools/plot_media_sweep_summary.py" \
  --sweep_summary_csv "$EXPERIMENT_DIR/sweep_summary.csv" \
  --out_dir "$EXPERIMENT_DIR/monitor_figures" >/dev/null

echo "[exp47] done: $EXPERIMENT_DIR/sweep_summary.csv"

