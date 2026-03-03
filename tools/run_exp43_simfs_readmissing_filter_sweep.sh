#!/usr/bin/env bash
set -euo pipefail

# Exp43: paper-style "filter dominates" signals under simulated_hybrid_file_system
#
# Goals:
# - Force missing point lookups (readmissing) so each op probes many filters
# - Shape DB into "many tables" (small SST + small buffers) so filter probes stack up
# - Measure filter-related total cost:
#     FilterTotal ~= read_filter_block_nanos + bloom_filter_maymatch_nanos
#   and correlate with probe counts (bloom_sst_hit/miss)
#
# Outputs:
# - per-case db_bench logs + tail probe CSV
# - analysis figures from tools/analyze_tail_probe_samples.py

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB_BENCH="${DB_BENCH:-$ROOT_DIR/build/db_bench}"

EXP_DATE="${EXP_DATE:-$(date +%Y%m%d)}"
GLOBAL_EXP_ID="${GLOBAL_EXP_ID:-43}"
EXPERIMENT_NAME="${EXPERIMENT_NAME:-simfs_readmissing_filter_sweep}"
RUN_TIME="${RUN_TIME:-$(date +%H%M%S)}"
EXPERIMENT_DIR="${EXPERIMENT_DIR:-$ROOT_DIR/experiment/${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}}"

TARGET_DB_GIB="${TARGET_DB_GIB:-0.20}"
KEY_SIZE="${KEY_SIZE:-16}"
VALUE_SIZE="${VALUE_SIZE:-1024}"
COMPRESSION_TYPE="${COMPRESSION_TYPE:-lz4}"
COMPRESSION_RATIO="${COMPRESSION_RATIO:-0.5}"

THREADS="${THREADS:-4}"
FILL_THREADS="${FILL_THREADS:-$THREADS}"
READS="${READS:-300000}"
READ_DURATION_SECONDS="${READ_DURATION_SECONDS:-0}"

# When set, reuse existing DB contents and only run the readmissing phase.
# This is useful for "judge experiments" like toggling --simulate_xp_bypass_base_io
# on the same DB shape without re-filling.
SKIP_FILL="${SKIP_FILL:-0}"

# Bloom bits sweep points (comma-separated). Use smaller bits to increase false
# positives and stress deeper probing.
BLOOM_BITS_LIST="${BLOOM_BITS_LIST:-4,8,10}"

# Cache sizes for runtime A/B. cache=0 emphasizes filter IO; cache>0 can surface CPU.
CACHE_SIZES="${CACHE_SIZES:-0,536870912}"
CACHE_INDEX_AND_FILTER_BLOCKS="${CACHE_INDEX_AND_FILTER_BLOCKS:-1}"
PIN_TOP_LEVEL_INDEX_AND_FILTER="${PIN_TOP_LEVEL_INDEX_AND_FILTER:-1}"

# Tail probe: capture read op tail samples.
TAIL_PROBE_THRESHOLD_US="${TAIL_PROBE_THRESHOLD_US:-200}"
TAIL_PROBE_MAX_SAMPLES="${TAIL_PROBE_MAX_SAMPLES:-20000}"
PERF_LEVEL="${PERF_LEVEL:-4}"

# Sample probe: periodic sampling (avoids tail-only selection bias).
SAMPLE_PROBE_INTERVAL_OPS="${SAMPLE_PROBE_INTERVAL_OPS:-1000}"
SAMPLE_PROBE_MAX_SAMPLES="${SAMPLE_PROBE_MAX_SAMPLES:-20000}"

# SimFS (pure redirect to tmpfs)
USE_TMPFS_REDIRECT="${USE_TMPFS_REDIRECT:-1}"
TMPFS_ROOT="${TMPFS_ROOT:-/dev/shm/nvm_tmpfs_root.exp43_simfs}"
SIMULATE_XP_BUSY_WAIT="${SIMULATE_XP_BUSY_WAIT:-1}"
SIMULATE_XP_BYPASS_BASE_IO="${SIMULATE_XP_BYPASS_BASE_IO:-0}"
READONLY_WHEN_BYPASS="${READONLY_WHEN_BYPASS:-1}"

DB_DIR="${DB_DIR:-/tmp/rocksdb_exp43_readmissing/db}"
WAL_DIR="${WAL_DIR:-/tmp/rocksdb_exp43_readmissing/wal}"

# DB-shaping knobs: make many small tables.
WRITE_BUFFER_SIZE="${WRITE_BUFFER_SIZE:-1048576}"           # 1MB
MAX_WRITE_BUFFER_NUMBER="${MAX_WRITE_BUFFER_NUMBER:-4}"
MIN_WRITE_BUFFER_NUMBER_TO_MERGE="${MIN_WRITE_BUFFER_NUMBER_TO_MERGE:-1}"
TARGET_FILE_SIZE_BASE="${TARGET_FILE_SIZE_BASE:-1048576}"   # 1MB
MAX_BYTES_FOR_LEVEL_BASE="${MAX_BYTES_FOR_LEVEL_BASE:-16777216}"  # 16MB
LEVEL0_FILE_NUM_COMPACTION_TRIGGER="${LEVEL0_FILE_NUM_COMPACTION_TRIGGER:-4}"
MAX_BACKGROUND_COMPACTIONS="${MAX_BACKGROUND_COMPACTIONS:-4}"

# Fill / shape mode knobs.
# - FILL_BENCH=fillrandom tends to spread keys across files/levels, increasing
#   the chance a read-miss probes multiple tables (Gate1).
# - FORCE_L0_OVERLAP=1 disables auto compactions during fill so L0 keeps many
#   overlapping files, forcing large filter-probe fanout (fastest way to make
#   probe_count >> 1 on a tight machine).
FILL_BENCH="${FILL_BENCH:-fillseq}"
FORCE_L0_OVERLAP="${FORCE_L0_OVERLAP:-0}"

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

mkdir -p "$EXPERIMENT_DIR"

NUM_KEYS="$(awk -v gib="$TARGET_DB_GIB" -v k="$KEY_SIZE" -v v="$VALUE_SIZE" \
  'BEGIN { bytes=gib*1024*1024*1024; per=k+v; n=int(bytes/per); if(n<1)n=1; print n }')"

echo "[exp43] EXPERIMENT_DIR=$EXPERIMENT_DIR"
echo "[exp43] target_db_gib=$TARGET_DB_GIB key_size=$KEY_SIZE value_size=$VALUE_SIZE -> num_keys=$NUM_KEYS"
echo "[exp43] bloom_bits_list=$BLOOM_BITS_LIST cache_sizes=$CACHE_SIZES"
echo "[exp43] tmpfs_redirect=$USE_TMPFS_REDIRECT tmpfs_root=$TMPFS_ROOT"

common_simfs_args=(
  --histogram=1
  --disable_wal=1
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
  --simulate_xp_busy_wait="$SIMULATE_XP_BUSY_WAIT"
)
if [[ "$USE_TMPFS_REDIRECT" == "1" ]]; then
  common_simfs_args+=(
    --simulate_xp_redirect_to_tmpfs=1
    --simulate_xp_tmpfs_root="$TMPFS_ROOT"
  )
fi

db_shape_args=(
  --key_size="$KEY_SIZE"
  --value_size="$VALUE_SIZE"
  --compression_type="$COMPRESSION_TYPE"
  --compression_ratio="$COMPRESSION_RATIO"
  --write_buffer_size="$WRITE_BUFFER_SIZE"
  --max_write_buffer_number="$MAX_WRITE_BUFFER_NUMBER"
  --min_write_buffer_number_to_merge="$MIN_WRITE_BUFFER_NUMBER_TO_MERGE"
  --target_file_size_base="$TARGET_FILE_SIZE_BASE"
  --max_bytes_for_level_base="$MAX_BYTES_FOR_LEVEL_BASE"
  --level0_file_num_compaction_trigger="$LEVEL0_FILE_NUM_COMPACTION_TRIGGER"
  --max_background_compactions="$MAX_BACKGROUND_COMPACTIONS"
)

fill_extra_args=()
wait_for_compaction_after_fill=1
if [[ "$FORCE_L0_OVERLAP" == "1" ]]; then
  # Keep L0 overlap large and stable: disable background compactions.
  fill_extra_args+=(--disable_auto_compactions=1)
  wait_for_compaction_after_fill=0
fi

read_duration_args=()
if [[ "$READ_DURATION_SECONDS" != "0" ]]; then
  read_duration_args+=(--duration="$READ_DURATION_SECONDS")
else
  read_duration_args+=(--reads="$READS")
fi

IFS=',' read -r -a BLOOM_BITS_ARRAY <<<"$BLOOM_BITS_LIST"
IFS=',' read -r -a CACHE_ARRAY <<<"$CACHE_SIZES"

for bloom_bits in "${BLOOM_BITS_ARRAY[@]}"; do
  bloom_bits="$(echo "$bloom_bits" | tr -d ' ')"
  if [[ -z "$bloom_bits" ]]; then
    continue
  fi

  run_tag="${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}_bloom${bloom_bits}_${RUN_TIME}"
  out_dir="$EXPERIMENT_DIR/run_results/$run_tag"
  mkdir -p "$out_dir"

  echo "[exp43][bloom=$bloom_bits] OUT_DIR=$out_dir"

  if [[ "$SKIP_FILL" != "1" ]]; then
    safe_rmtree "$DB_DIR"
    safe_rmtree "$WAL_DIR"
    mkdir -p "$WAL_DIR"
    if [[ "$USE_TMPFS_REDIRECT" == "1" ]]; then
      safe_rmtree "$TMPFS_ROOT"
    fi
  else
    mkdir -p "$WAL_DIR"
  fi

  if [[ "$SKIP_FILL" != "1" ]]; then
    echo "[exp43][bloom=$bloom_bits] fill DB"
    fill_cmd=(
      "$DB_BENCH"
      --db="$DB_DIR"
      --wal_dir="$WAL_DIR"
      --benchmarks="${FILL_BENCH}",stats
      --statistics
      --num="$NUM_KEYS"
      --threads="$FILL_THREADS"
      --bloom_bits="$bloom_bits"
      "${common_simfs_args[@]}"
      "${db_shape_args[@]}"
      "${fill_extra_args[@]}"
    )
    printf "%q " "${fill_cmd[@]}" >"$out_dir/01_fillseq.cmd"
    printf "\n" >>"$out_dir/01_fillseq.cmd"
    "${fill_cmd[@]}" 2>&1 | tee "$out_dir/01_fillseq.log"
  else
    echo "[exp43][bloom=$bloom_bits] SKIP_FILL=1 (reuse existing DB at $DB_DIR)"
  fi

  if [[ "$SKIP_FILL" != "1" && "$wait_for_compaction_after_fill" == "1" ]]; then
    echo "[exp43][bloom=$bloom_bits] waitforcompaction (stabilize DB shape)"
    wait_cmd=(
      "$DB_BENCH"
      --db="$DB_DIR"
      --wal_dir="$WAL_DIR"
      --use_existing_db=1
      --benchmarks=waitforcompaction,stats
      --statistics
      --threads=1
      --num="$NUM_KEYS"
      --bloom_bits="$bloom_bits"
      "${common_simfs_args[@]}"
      "${db_shape_args[@]}"
    )
    printf "%q " "${wait_cmd[@]}" >"$out_dir/01b_waitforcompaction.cmd"
    printf "\n" >>"$out_dir/01b_waitforcompaction.cmd"
    "${wait_cmd[@]}" 2>&1 | tee "$out_dir/01b_waitforcompaction.log"
  elif [[ "$SKIP_FILL" != "1" ]]; then
    echo "[exp43][bloom=$bloom_bits] skip waitforcompaction (FORCE_L0_OVERLAP=1)"
  fi

  for cache_size in "${CACHE_ARRAY[@]}"; do
    cache_size="$(echo "$cache_size" | tr -d ' ')"
    if [[ -z "$cache_size" ]]; then
      continue
    fi

    echo "[exp43][bloom=$bloom_bits][cache=$cache_size] readmissing"
    tail_csv="$out_dir/02_readmissing_cache_${cache_size}.tail_probe.csv"
    sample_csv="$out_dir/02_readmissing_cache_${cache_size}.sample_probe.csv"
    analysis_tail_dir="$out_dir/analysis_readmissing_tail_cache_${cache_size}"
    analysis_sample_dir="$out_dir/analysis_readmissing_sample_cache_${cache_size}"
    mkdir -p "$analysis_tail_dir"
    mkdir -p "$analysis_sample_dir"

    extra_cache_args=(--cache_size="$cache_size")
    if [[ "$cache_size" != "0" && "$CACHE_INDEX_AND_FILTER_BLOCKS" == "1" ]]; then
      extra_cache_args+=(--cache_index_and_filter_blocks=1)
      if [[ "$PIN_TOP_LEVEL_INDEX_AND_FILTER" == "1" ]]; then
        extra_cache_args+=(--pin_top_level_index_and_filter=1)
      fi
    fi

    readonly_args=()
    if [[ "$SIMULATE_XP_BYPASS_BASE_IO" == "1" && "$READONLY_WHEN_BYPASS" == "1" ]]; then
      readonly_args+=(--readonly=1)
    fi

    read_cmd=(
      "$DB_BENCH"
      --db="$DB_DIR"
      --wal_dir="$WAL_DIR"
      --use_existing_db=1
      --benchmarks=readmissing,stats
      --statistics
      --num="$NUM_KEYS"
      --threads="$THREADS"
      --bloom_bits="$bloom_bits"
      --simulate_xp_bypass_base_io="$SIMULATE_XP_BYPASS_BASE_IO"
      "${read_duration_args[@]}"
      "${common_simfs_args[@]}"
      "${db_shape_args[@]}"
      "${extra_cache_args[@]}"
      "${fill_extra_args[@]}"
      "${readonly_args[@]}"
      --tail_probe_output="$tail_csv"
      --tail_probe_op=read
      --tail_probe_threshold_us="$TAIL_PROBE_THRESHOLD_US"
      --tail_probe_max_samples="$TAIL_PROBE_MAX_SAMPLES"
      --tail_probe_case_label="cache${cache_size}"
      --tail_probe_scenario="readmissing"
      --sample_probe_output="$sample_csv"
      --sample_probe_op=read
      --sample_probe_interval_ops="$SAMPLE_PROBE_INTERVAL_OPS"
      --sample_probe_max_samples="$SAMPLE_PROBE_MAX_SAMPLES"
      --sample_probe_case_label="cache${cache_size}"
      --sample_probe_scenario="readmissing_sample"
    )
    printf "%q " "${read_cmd[@]}" >"$out_dir/02_readmissing_cache_${cache_size}.cmd"
    printf "\n" >>"$out_dir/02_readmissing_cache_${cache_size}.cmd"
    "${read_cmd[@]}" 2>&1 | tee "$out_dir/02_readmissing_cache_${cache_size}.log"

    python3 "$ROOT_DIR/tools/analyze_tail_probe_samples.py" \
      --samples_csv "$tail_csv" \
      --out_dir "$analysis_tail_dir" \
      --label "cache${cache_size}" \
      --phase "readmissing_bloom${bloom_bits}" \
      --scenario "readmissing" \
      --threshold_us "$TAIL_PROBE_THRESHOLD_US" \
      --kind tail

    python3 "$ROOT_DIR/tools/analyze_tail_probe_samples.py" \
      --samples_csv "$sample_csv" \
      --out_dir "$analysis_sample_dir" \
      --label "cache${cache_size}" \
      --phase "readmissing_bloom${bloom_bits}_sample" \
      --scenario "readmissing_sample" \
      --threshold_us 0 \
      --kind sample
  done
done

echo "[exp43] done: $EXPERIMENT_DIR"
