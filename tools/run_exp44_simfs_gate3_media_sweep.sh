#!/usr/bin/env bash
set -euo pipefail

# Exp44: Gate3 "media speed sweep" to answer:
#   - Inside FilterTotal, is the cost mostly "wait" (media/queue) or "do" (CPU/memory)?
#   - As the simulated media gets faster, does CPU (FilterCPU / software stages)
#     become more salient (paper-style trend)?
#
# We keep DB shape fixed (FORCE_L0_OVERLAP=1) and focus on:
#   - readmissing (miss-heavy point lookups)
#   - cache=500MB (closer to CPU-dominance observation)
#   - sample probe only (avoid tail selection bias / keep run time short)
#   - bypass_base_io sweep (0 vs 1) for a clean "judge" variant
#
# Outputs per point:
#   - 02_readmissing_cache_536870912.sample_probe.csv
#   - analysis_readmissing_sample_cache_536870912/*
#   - sweep_summary.csv (across all points)
#
# Note: This script reuses the exp43 DB dir by default so it can run without
# re-filling if you already built a stable 4GB logical DB. Set RESET_DB=1 to
# rebuild.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB_BENCH="${DB_BENCH:-$ROOT_DIR/build/db_bench}"

EXP_DATE="${EXP_DATE:-$(date +%Y%m%d)}"
GLOBAL_EXP_ID="${GLOBAL_EXP_ID:-44}"
EXPERIMENT_NAME="${EXPERIMENT_NAME:-gate3_media_sweep}"
RUN_TIME="${RUN_TIME:-$(date +%H%M%S)}"
EXPERIMENT_DIR="${EXPERIMENT_DIR:-$ROOT_DIR/experiment/${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}}"

# DB + data shape (match exp43 defaults unless overridden).
TARGET_DB_GIB="${TARGET_DB_GIB:-4.0}"
KEY_SIZE="${KEY_SIZE:-16}"
VALUE_SIZE="${VALUE_SIZE:-1024}"
COMPRESSION_TYPE="${COMPRESSION_TYPE:-lz4}"
COMPRESSION_RATIO="${COMPRESSION_RATIO:-0.10}"

THREADS="${THREADS:-4}"
FILL_THREADS="${FILL_THREADS:-$THREADS}"
READS_PER_THREAD="${READS_PER_THREAD:-30000}"

# Sweep points.
XP_LATENCY_LIST_NS="${XP_LATENCY_LIST_NS:-75,25,8}"
BYPASS_LIST="${BYPASS_LIST:-0,1}"

# Cache fixed at 500MB for Gate3.
CACHE_SIZE="${CACHE_SIZE:-536870912}"
CACHE_INDEX_AND_FILTER_BLOCKS="${CACHE_INDEX_AND_FILTER_BLOCKS:-1}"
PIN_TOP_LEVEL_INDEX_AND_FILTER="${PIN_TOP_LEVEL_INDEX_AND_FILTER:-1}"

# Probing.
PERF_LEVEL="${PERF_LEVEL:-4}"
SAMPLE_PROBE_INTERVAL_OPS="${SAMPLE_PROBE_INTERVAL_OPS:-200}"
SAMPLE_PROBE_MAX_SAMPLES="${SAMPLE_PROBE_MAX_SAMPLES:-20000}"

# SimFS: tmpfs redirect + busy-wait to reduce scheduling noise.
USE_TMPFS_REDIRECT="${USE_TMPFS_REDIRECT:-1}"
TMPFS_ROOT="${TMPFS_ROOT:-/dev/shm/nvm_tmpfs_root.exp43_simfs}"
SIMULATE_XP_BUSY_WAIT="${SIMULATE_XP_BUSY_WAIT:-1}"
READONLY_WHEN_BYPASS="${READONLY_WHEN_BYPASS:-1}"

# DB locations: default to exp43 paths for reuse.
DB_DIR="${DB_DIR:-/tmp/rocksdb_exp43_readmissing/db}"
WAL_DIR="${WAL_DIR:-/tmp/rocksdb_exp43_readmissing/wal}"

# If 1, wipe DB + tmpfs root and re-fill before sweeping.
RESET_DB="${RESET_DB:-0}"

# Shape into extreme overlap (Gate1).
FILL_BENCH="${FILL_BENCH:-fillrandom}"
FORCE_L0_OVERLAP="${FORCE_L0_OVERLAP:-1}"

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

echo "[exp44] EXPERIMENT_DIR=$EXPERIMENT_DIR"
echo "[exp44] db=$DB_DIR wal=$WAL_DIR tmpfs_root=$TMPFS_ROOT"
echo "[exp44] target_db_gib=$TARGET_DB_GIB key_size=$KEY_SIZE value_size=$VALUE_SIZE -> num_keys=$NUM_KEYS"
echo "[exp44] sweep xp_latency_ns=[$XP_LATENCY_LIST_NS], bypass=[$BYPASS_LIST]"

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

db_shape_args=(
  --key_size="$KEY_SIZE"
  --value_size="$VALUE_SIZE"
  --compression_type="$COMPRESSION_TYPE"
  --compression_ratio="$COMPRESSION_RATIO"
  --write_buffer_size="${WRITE_BUFFER_SIZE:-131072}"
  --max_write_buffer_number=4
  --min_write_buffer_number_to_merge=1
  --target_file_size_base="${TARGET_FILE_SIZE_BASE:-131072}"
  --max_bytes_for_level_base=16777216
  --level0_file_num_compaction_trigger=4
  --max_background_compactions=4
  --bloom_bits=10
)

fill_extra_args=()
if [[ "$FORCE_L0_OVERLAP" == "1" ]]; then
  fill_extra_args+=(--disable_auto_compactions=1)
fi

cache_args=(
  --cache_size="$CACHE_SIZE"
  --cache_index_and_filter_blocks="$CACHE_INDEX_AND_FILTER_BLOCKS"
  --pin_top_level_index_and_filter="$PIN_TOP_LEVEL_INDEX_AND_FILTER"
)

ensure_db() {
  if [[ "$RESET_DB" == "1" ]]; then
    safe_rmtree "$DB_DIR"
    safe_rmtree "$WAL_DIR"
    mkdir -p "$WAL_DIR"
    if [[ "$USE_TMPFS_REDIRECT" == "1" ]]; then
      safe_rmtree "$TMPFS_ROOT"
    fi
  fi

  if [[ -f "$DB_DIR/CURRENT" ]]; then
    return 0
  fi

  echo "[exp44] fill DB (FORCE_L0_OVERLAP=$FORCE_L0_OVERLAP)"
  mkdir -p "$WAL_DIR"
  local fill_cmd=(
    "$DB_BENCH"
    --db="$DB_DIR"
    --wal_dir="$WAL_DIR"
    --benchmarks="${FILL_BENCH}",stats
    --statistics
    --num="$NUM_KEYS"
    --threads="$FILL_THREADS"
    "${common_simfs_args_base[@]}"
    --simulate_xp_latency_ns=75
    "${db_shape_args[@]}"
    "${fill_extra_args[@]}"
  )
  mkdir -p "$EXPERIMENT_DIR/run_results"
  printf "%q " "${fill_cmd[@]}" >"$EXPERIMENT_DIR/00_fill_db.cmd"
  printf "\n" >>"$EXPERIMENT_DIR/00_fill_db.cmd"
  "${fill_cmd[@]}" 2>&1 | tee "$EXPERIMENT_DIR/00_fill_db.log"
}

ensure_db

mkdir -p "$EXPERIMENT_DIR/run_results"
summary_csv="$EXPERIMENT_DIR/sweep_points.csv"
echo "run_tag,xp_latency_ns,bypass_base_io,run_dir" >"$summary_csv"

IFS=',' read -r -a XP_ARRAY <<<"$XP_LATENCY_LIST_NS"
IFS=',' read -r -a BYPASS_ARRAY <<<"$BYPASS_LIST"

for bypass in "${BYPASS_ARRAY[@]}"; do
  bypass="$(echo "$bypass" | tr -d ' ')"
  if [[ -z "$bypass" ]]; then
    continue
  fi
  for xp_ns in "${XP_ARRAY[@]}"; do
    xp_ns="$(echo "$xp_ns" | tr -d ' ')"
    if [[ -z "$xp_ns" ]]; then
      continue
    fi

    run_tag="${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}_xplat${xp_ns}_bypass${bypass}_${RUN_TIME}"
    out_dir="$EXPERIMENT_DIR/run_results/$run_tag"
    mkdir -p "$out_dir"
    echo "[exp44] RUN $run_tag"

    readonly_args=()
    if [[ "$bypass" == "1" && "$READONLY_WHEN_BYPASS" == "1" ]]; then
      readonly_args+=(--readonly=1)
    fi

    common_simfs_args=("${common_simfs_args_base[@]}" --simulate_xp_latency_ns="$xp_ns")

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
      --reads="$((READS_PER_THREAD * THREADS))"
      --simulate_xp_bypass_base_io="$bypass"
      "${common_simfs_args[@]}"
      "${db_shape_args[@]}"
      "${fill_extra_args[@]}"
      "${cache_args[@]}"
      "${readonly_args[@]}"
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
      --phase "readmissing_xplat${xp_ns}_bypass${bypass}" \
      --scenario "readmissing_sample" \
      --threshold_us 0 \
      --kind sample

    echo "$run_tag,$xp_ns,$bypass,$out_dir" >>"$summary_csv"
  done
done

python3 "$ROOT_DIR/tools/summarize_exp44_media_sweep.py" \
  --sweep_points_csv "$summary_csv" \
  --out_csv "$EXPERIMENT_DIR/sweep_summary.csv"

echo "[exp44] done: $EXPERIMENT_DIR"
