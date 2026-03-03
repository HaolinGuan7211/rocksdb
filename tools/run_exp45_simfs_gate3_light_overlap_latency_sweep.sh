#!/usr/bin/env bash
set -euo pipefail

# Exp45: Gate3 "light overlap" + media-latency sweep (paper-aligned)
#
# Motivation:
# - The extreme hi-probe regime (probe ~10k) can inflate software stack cost to
#   tens of milliseconds and move the system away from the paper's typical
#   operating point.
# - Here we aim for a more realistic "many tables but not exploding" shape:
#     filter_probe_total ~ 10..200
# - Then we sweep simulated media latency across ns→us to visualize the classic
#   trend: as media gets faster, CPU components (FilterCPU + software search)
#   become more salient.
#
# Key design choices:
# - Keep cache=500MB and cache index+filter blocks to avoid frequent filter/index IO.
# - Use readmissing with small bloom_bits to introduce controlled false-positives
#   so there is still real data-block IO pressure to respond to media latency.
# - Use sample probe only (typical op trend) and summarize by probe bucket.
#
# Outputs:
# - run_results/<run_tag>/... sample_probe.csv, logs, analysis figures
# - sweep_points.csv + sweep_summary.csv

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB_BENCH="${DB_BENCH:-$ROOT_DIR/build/db_bench}"

EXP_DATE="${EXP_DATE:-$(date +%Y%m%d)}"
GLOBAL_EXP_ID="${GLOBAL_EXP_ID:-45}"
EXPERIMENT_NAME="${EXPERIMENT_NAME:-gate3_light_overlap_latency_sweep}"
RUN_TIME="${RUN_TIME:-$(date +%H%M%S)}"
EXPERIMENT_DIR="${EXPERIMENT_DIR:-$ROOT_DIR/experiment/${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}}"

# DB sizing: keep within the 4GB budget; on /dev/shm this requires high compression.
TARGET_DB_GIB="${TARGET_DB_GIB:-4.0}"
KEY_SIZE="${KEY_SIZE:-16}"
VALUE_SIZE="${VALUE_SIZE:-1024}"
COMPRESSION_TYPE="${COMPRESSION_TYPE:-lz4}"
COMPRESSION_RATIO="${COMPRESSION_RATIO:-0.10}"

# DB shape knobs: aim for "light overlap" (tens of L0 files, not thousands).
WRITE_BUFFER_SIZE="${WRITE_BUFFER_SIZE:-8388608}"         # 8MB
TARGET_FILE_SIZE_BASE="${TARGET_FILE_SIZE_BASE:-8388608}" # 8MB
FORCE_L0_OVERLAP="${FORCE_L0_OVERLAP:-1}"
FILL_BENCH="${FILL_BENCH:-fillrandom}"
FILL_THREADS="${FILL_THREADS:-4}"

# Runtime knobs.
THREADS="${THREADS:-1}"
READS_PER_POINT="${READS_PER_POINT:-50000}"
CACHE_SIZE="${CACHE_SIZE:-536870912}" # 500MB

# Sweep knobs.
XP_LATENCY_LIST_NS="${XP_LATENCY_LIST_NS:-75,750,7500,75000}"
# Use low bloom bits to increase false positives so misses still trigger some IO.
BLOOM_BITS_LIST="${BLOOM_BITS_LIST:-2,4,10}"

# Probing.
PERF_LEVEL="${PERF_LEVEL:-4}"
SAMPLE_PROBE_INTERVAL_OPS="${SAMPLE_PROBE_INTERVAL_OPS:-200}"
SAMPLE_PROBE_MAX_SAMPLES="${SAMPLE_PROBE_MAX_SAMPLES:-20000}"

# SimFS.
USE_TMPFS_REDIRECT="${USE_TMPFS_REDIRECT:-1}"
TMPFS_ROOT="${TMPFS_ROOT:-/dev/shm/nvm_tmpfs_root.exp45_gate3_light}"
SIMULATE_XP_BUSY_WAIT="${SIMULATE_XP_BUSY_WAIT:-0}" # default to sleep-based wait for paper-style "wait vs cpu"

DB_DIR="${DB_DIR:-/tmp/rocksdb_exp45_gate3_light/db}"
WAL_DIR="${WAL_DIR:-/tmp/rocksdb_exp45_gate3_light/wal}"
RESET_DB="${RESET_DB:-1}"

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

echo "[exp45] EXPERIMENT_DIR=$EXPERIMENT_DIR"
echo "[exp45] target_db_gib=$TARGET_DB_GIB key_size=$KEY_SIZE value_size=$VALUE_SIZE -> num_keys=$NUM_KEYS"
echo "[exp45] shape: write_buffer_size=$WRITE_BUFFER_SIZE target_file_size_base=$TARGET_FILE_SIZE_BASE force_l0_overlap=$FORCE_L0_OVERLAP fill=$FILL_BENCH"
echo "[exp45] sweep: xp_latency_ns=[$XP_LATENCY_LIST_NS], bloom_bits=[$BLOOM_BITS_LIST]"

common_simfs_args_base=(
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
)
if [[ "$USE_TMPFS_REDIRECT" == "1" ]]; then
  common_simfs_args_base+=(
    --simulate_xp_redirect_to_tmpfs=1
    --simulate_xp_tmpfs_root="$TMPFS_ROOT"
  )
fi

db_shape_args_base=(
  --key_size="$KEY_SIZE"
  --value_size="$VALUE_SIZE"
  --compression_type="$COMPRESSION_TYPE"
  --compression_ratio="$COMPRESSION_RATIO"
  --write_buffer_size="$WRITE_BUFFER_SIZE"
  --max_write_buffer_number=4
  --min_write_buffer_number_to_merge=1
  --target_file_size_base="$TARGET_FILE_SIZE_BASE"
  --max_bytes_for_level_base=268435456   # 256MB
  --level0_file_num_compaction_trigger=8
  --max_background_compactions=4
)

fill_extra_args=()
if [[ "$FORCE_L0_OVERLAP" == "1" ]]; then
  fill_extra_args+=(--disable_auto_compactions=1)
fi

cache_args=(
  --cache_size="$CACHE_SIZE"
  --cache_index_and_filter_blocks=1
  --pin_top_level_index_and_filter=1
)

if [[ "$RESET_DB" == "1" ]]; then
  safe_rmtree "$DB_DIR"
  safe_rmtree "$WAL_DIR"
  mkdir -p "$WAL_DIR"
  if [[ "$USE_TMPFS_REDIRECT" == "1" ]]; then
    safe_rmtree "$TMPFS_ROOT"
  fi
fi

if [[ ! -f "$DB_DIR/CURRENT" ]]; then
  echo "[exp45] fill DB"
  mkdir -p "$WAL_DIR"
  fill_cmd=(
    "$DB_BENCH"
    --db="$DB_DIR"
    --wal_dir="$WAL_DIR"
    --benchmarks="$FILL_BENCH",stats
    --statistics
    --num="$NUM_KEYS"
    --threads="$FILL_THREADS"
    --bloom_bits=10
    "${common_simfs_args_base[@]}"
    --simulate_xp_latency_ns=750
    "${db_shape_args_base[@]}"
    "${fill_extra_args[@]}"
  )
  printf "%q " "${fill_cmd[@]}" >"$EXPERIMENT_DIR/00_fill_db.cmd"
  printf "\n" >>"$EXPERIMENT_DIR/00_fill_db.cmd"
  "${fill_cmd[@]}" 2>&1 | tee "$EXPERIMENT_DIR/00_fill_db.log"
else
  echo "[exp45] reuse existing DB at $DB_DIR (RESET_DB=0)"
fi

mkdir -p "$EXPERIMENT_DIR/run_results"
points_csv="$EXPERIMENT_DIR/sweep_points.csv"
echo "run_tag,xp_latency_ns,bloom_bits,run_dir" >"$points_csv"

IFS=',' read -r -a XP_ARRAY <<<"$XP_LATENCY_LIST_NS"
IFS=',' read -r -a BLOOM_ARRAY <<<"$BLOOM_BITS_LIST"

for bloom_bits in "${BLOOM_ARRAY[@]}"; do
  bloom_bits="$(echo "$bloom_bits" | tr -d ' ')"
  if [[ -z "$bloom_bits" ]]; then
    continue
  fi
  for xp_ns in "${XP_ARRAY[@]}"; do
    xp_ns="$(echo "$xp_ns" | tr -d ' ')"
    if [[ -z "$xp_ns" ]]; then
      continue
    fi

    run_tag="${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}_bloom${bloom_bits}_xplat${xp_ns}_${RUN_TIME}"
    out_dir="$EXPERIMENT_DIR/run_results/$run_tag"
    mkdir -p "$out_dir"

    sample_csv="$out_dir/02_readmissing_cache_${CACHE_SIZE}.sample_probe.csv"
    analysis_dir="$out_dir/analysis_readmissing_sample_cache_${CACHE_SIZE}"

    common_simfs_args=("${common_simfs_args_base[@]}" --simulate_xp_latency_ns="$xp_ns")

    read_cmd=(
      "$DB_BENCH"
      --db="$DB_DIR"
      --wal_dir="$WAL_DIR"
      --use_existing_db=1
      --benchmarks=readmissing,stats
      --statistics
      --num="$NUM_KEYS"
      --threads="$THREADS"
      --reads="$READS_PER_POINT"
      --bloom_bits="$bloom_bits"
      "${common_simfs_args[@]}"
      "${db_shape_args_base[@]}"
      "${fill_extra_args[@]}"
      "${cache_args[@]}"
      --sample_probe_output="$sample_csv"
      --sample_probe_op=read
      --sample_probe_interval_ops="$SAMPLE_PROBE_INTERVAL_OPS"
      --sample_probe_max_samples="$SAMPLE_PROBE_MAX_SAMPLES"
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
      --phase "readmissing_bloom${bloom_bits}_xplat${xp_ns}" \
      --scenario "readmissing_sample" \
      --threshold_us 0 \
      --kind sample

    echo "$run_tag,$xp_ns,$bloom_bits,$out_dir" >>"$points_csv"
  done
done

python3 "$ROOT_DIR/tools/summarize_exp44_media_sweep.py" \
  --sweep_points_csv "$points_csv" \
  --out_csv "$EXPERIMENT_DIR/sweep_summary.csv"

echo "[exp45] done: $EXPERIMENT_DIR"

