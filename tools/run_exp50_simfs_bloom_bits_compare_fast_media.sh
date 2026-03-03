#!/usr/bin/env bash
set -euo pipefail

# Exp50: Compare bloom_bits impact at the "fast media" point under mmap-base-IO.
#
# Motivation:
# - Under fast media (ns-scale), we observed FilterTotal share plateauing ~41-42%.
# - We want to see how much of that depends on Bloom FP rate (bloom_bits).
#
# Method:
# - Rebuild DB twice with bloom_bits in {2,10} (filters are baked into SSTs).
# - For each DB, run readmissing under:
#     simulate_xp_mmap_base_io=1 (remove OS/FS syscall floor)
#     simulate_xp_latency_ns=8 (fastest point)
#     cache=500MB, overlap-ish DB shape (wbuf/tfile=128MB, auto_compactions=0)
# - Collect sample-probe CSV, analyze, and summarize into one table.
#
# Outputs:
# - sweep_summary.csv (same schema as exp44/46/49)
# - bloom_compare_summary.csv (one row per bloom_bits for probe_bucket=16+)
# - monitor_figures/ (simple plots; single-point per curve)

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB_BENCH="${DB_BENCH:-$ROOT_DIR/build/db_bench}"

EXP_DATE="${EXP_DATE:-$(date +%Y%m%d)}"
GLOBAL_EXP_ID="${GLOBAL_EXP_ID:-50}"
EXPERIMENT_NAME="${EXPERIMENT_NAME:-bloom_bits_compare_fast_media}"
RUN_TIME="${RUN_TIME:-$(date +%H%M%S)}"
EXPERIMENT_DIR="${EXPERIMENT_DIR:-$ROOT_DIR/experiment/${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}_${RUN_TIME}}"

TARGET_DB_GIB="${TARGET_DB_GIB:-4.0}"
KEY_SIZE="${KEY_SIZE:-16}"
VALUE_SIZE="${VALUE_SIZE:-1024}"
COMPRESSION_TYPE="${COMPRESSION_TYPE:-lz4}"
COMPRESSION_RATIO="${COMPRESSION_RATIO:-0.10}"

# Compare points.
BLOOM_BITS_LIST="${BLOOM_BITS_LIST:-2,10}"

# Fast media point.
XP_LATENCY_NS="${XP_LATENCY_NS:-8}"

# Workload.
THREADS="${THREADS:-1}"
READS="${READS:-5000}"
SAMPLE_INTERVAL_OPS="${SAMPLE_INTERVAL_OPS:-10}"
SAMPLE_MAX_SAMPLES="${SAMPLE_MAX_SAMPLES:-20000}"

# Cache.
CACHE_SIZE="${CACHE_SIZE:-536870912}"
PERF_LEVEL="${PERF_LEVEL:-4}"

# DB shape (match exp46 chosen point).
WRITE_BUFFER_SIZE="${WRITE_BUFFER_SIZE:-134217728}"
TARGET_FILE_SIZE_BASE="${TARGET_FILE_SIZE_BASE:-134217728}"
DISABLE_AUTO_COMPACTIONS="${DISABLE_AUTO_COMPACTIONS:-1}"
FILL_THREADS="${FILL_THREADS:-4}"

# SimFS.
TMPFS_ROOT="${TMPFS_ROOT:-/dev/shm/nvm_tmpfs_root.exp50_bloom_compare}"
SIMULATE_XP_BUSY_WAIT="${SIMULATE_XP_BUSY_WAIT:-0}"
DB_DIR="${DB_DIR:-/tmp/rocksdb_exp50_bloom_compare/db}"
WAL_DIR="${WAL_DIR:-/tmp/rocksdb_exp50_bloom_compare/wal}"

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

mkdir -p "$EXPERIMENT_DIR/run_results"

NUM_KEYS="$(awk -v gib="$TARGET_DB_GIB" -v k="$KEY_SIZE" -v v="$VALUE_SIZE" \
  'BEGIN { bytes=gib*1024*1024*1024; per=k+v; n=int(bytes/per); if(n<1)n=1; print n }')"

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
  --simulate_xp_bypass_base_io=0
  --simulate_xp_mmap_base_io=1
  --simulate_xp_latency_ns="$XP_LATENCY_NS"
)

cache_args=(
  --cache_size="$CACHE_SIZE"
  --cache_index_and_filter_blocks=1
  --pin_top_level_index_and_filter=1
)

points_csv="$EXPERIMENT_DIR/sweep_points.csv"
echo "run_tag,xp_latency_ns,bloom_bits,run_dir" >"$points_csv"

IFS=',' read -r -a BLOOM_ARRAY <<<"$BLOOM_BITS_LIST"
for bloom_bits in "${BLOOM_ARRAY[@]}"; do
  bloom_bits="$(echo "$bloom_bits" | tr -d ' ')"
  tag="${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}_bloom${bloom_bits}_xplat${XP_LATENCY_NS}_${RUN_TIME}"
  out_dir="$EXPERIMENT_DIR/run_results/$tag"
  mkdir -p "$out_dir"

  safe_rmtree "$DB_DIR"
  safe_rmtree "$WAL_DIR"
  mkdir -p "$WAL_DIR"
  safe_rmtree "$TMPFS_ROOT"

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
    --bloom_bits="$bloom_bits"
  )
  if [[ "$DISABLE_AUTO_COMPACTIONS" == "1" ]]; then
    db_shape_args+=(--disable_auto_compactions=1)
  fi

  echo "[exp50] build DB bloom_bits=$bloom_bits num_keys=$NUM_KEYS"
  fill_cmd=(
    "$DB_BENCH"
    --db="$DB_DIR"
    --wal_dir="$WAL_DIR"
    --benchmarks=fillrandom,stats
    --statistics
    --num="$NUM_KEYS"
    --threads="$FILL_THREADS"
    "${common_simfs_args[@]}"
    "${db_shape_args[@]}"
  )
  printf "%q " "${fill_cmd[@]}" >"$out_dir/00_fill.cmd"
  printf "\n" >>"$out_dir/00_fill.cmd"
  "${fill_cmd[@]}" 2>&1 | tee "$out_dir/00_fill.log"

  sample_csv="$out_dir/02_readmissing_cache_${CACHE_SIZE}.sample_probe.csv"
  analysis_dir="$out_dir/analysis_readmissing_sample_cache_${CACHE_SIZE}"

  echo "[exp50] readmissing bloom_bits=$bloom_bits xp_latency_ns=$XP_LATENCY_NS"
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
    # NOTE: bloom_bits here does NOT rebuild filters (filters are in SST), but
    # keeping it consistent avoids confusion when dumping options/stats.
    --bloom_bits="$bloom_bits"
    "${common_simfs_args[@]}"
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
    --phase "bloom${bloom_bits}_xplat${XP_LATENCY_NS}_mmap1" \
    --scenario "readmissing_sample" \
    --threshold_us 0 \
    --kind sample >/dev/null

  echo "$tag,$XP_LATENCY_NS,$bloom_bits,$out_dir" >>"$points_csv"
done

python3 "$ROOT_DIR/tools/summarize_exp44_media_sweep.py" \
  --sweep_points_csv "$points_csv" \
  --out_csv "$EXPERIMENT_DIR/sweep_summary.csv" >/dev/null

python3 "$ROOT_DIR/tools/plot_media_sweep_summary.py" \
  --sweep_summary_csv "$EXPERIMENT_DIR/sweep_summary.csv" \
  --out_dir "$EXPERIMENT_DIR/monitor_figures" >/dev/null

python3 "$ROOT_DIR/tools/plot_bloom_compare_fast_media.py" \
  --sweep_summary_csv "$EXPERIMENT_DIR/sweep_summary.csv" \
  --sweep_points_csv "$points_csv" \
  --out_dir "$EXPERIMENT_DIR/monitor_figures" >/dev/null

cp -f "$EXPERIMENT_DIR/monitor_figures/bloom_compare_table.csv" \
  "$EXPERIMENT_DIR/bloom_compare_summary.csv"

echo "[exp50] done: $EXPERIMENT_DIR/bloom_compare_summary.csv"
echo "[exp50] done: $EXPERIMENT_DIR/sweep_summary.csv"
