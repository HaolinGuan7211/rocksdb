#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DB_BENCH="${DB_BENCH:-$ROOT_DIR/build/db_bench}"
ANALYZE_SCRIPT="${ANALYZE_SCRIPT:-$ROOT_DIR/tools/nvm_fs/analyze_envfs_xp_bench.py}"

if [[ ! -x "$DB_BENCH" ]]; then
  echo "missing db_bench: $DB_BENCH" >&2
  echo "build with: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target db_bench -j8" >&2
  exit 1
fi
if [[ ! -f "$ANALYZE_SCRIPT" ]]; then
  echo "missing analyzer: $ANALYZE_SCRIPT" >&2
  exit 1
fi

RUN_TAG="${RUN_TAG:-envfs_xp_$(date +%Y%m%d_%H%M%S)}"
RUN_ROOT="${RUN_ROOT:-$ROOT_DIR/experiment}"
OUT_DIR="${OUT_DIR:-$RUN_ROOT/$RUN_TAG}"
DB_ROOT="${DB_ROOT:-/tmp/rocksdb_envfs_xp_$RUN_TAG}"
MODES="${MODES:-sim,base}" # sim | base
PMEM_REFERENCE_CSV="${PMEM_REFERENCE_CSV:-}"

KEY_SIZE="${KEY_SIZE:-16}"
FILL_VALUE_SIZE="${FILL_VALUE_SIZE:-256}"
NUM_KEYS="${NUM_KEYS:-2000000}"
READS_LAT="${READS_LAT:-500000}"
READS_BW="${READS_BW:-4000000}"
WRITES_LAT="${WRITES_LAT:-500000}"
WRITES_BW="${WRITES_BW:-3000000}"
THREADS_FILL="${THREADS_FILL:-4}"
THREADS_LAT="${THREADS_LAT:-1}"
THREADS_BW="${THREADS_BW:-16}"
CACHE_SIZE="${CACHE_SIZE:-0}"
COMPRESSION_TYPE="${COMPRESSION_TYPE:-none}"
USE_DIRECT="${USE_DIRECT:-true}"

XP_LINE_BYTES="${XP_LINE_BYTES:-256}"
XP_BUFFER_BYTES="${XP_BUFFER_BYTES:-16384}"
XP_LATENCY_NS="${XP_LATENCY_NS:-300}"
XP_RPQ_DEPTH="${XP_RPQ_DEPTH:-64}"
XP_WPQ_DEPTH="${XP_WPQ_DEPTH:-64}"
XP_WPQ_SUBMIT_NS="${XP_WPQ_SUBMIT_NS:-100}"
XP_PREFETCH_HIT_NS="${XP_PREFETCH_HIT_NS:-120}"
XP_ENABLE_PREFETCH="${XP_ENABLE_PREFETCH:-true}"
XP_PATH_PREFIX="${XP_PATH_PREFIX:-}"
XP_LEVELS="${XP_LEVELS:-0,1,2,3,4}"

mkdir -p "$OUT_DIR" "$OUT_DIR/logs" "$OUT_DIR/stats" "$DB_ROOT"

manifest="$OUT_DIR/case_manifest.csv"
echo "mode,case,benchmark,threads,value_size,log_file,stats_file" >"$manifest"

common_opts=(
  "--key_size=$KEY_SIZE"
  "--compression_type=$COMPRESSION_TYPE"
  "--use_direct_reads=$USE_DIRECT"
  "--use_direct_io_for_flush_and_compaction=$USE_DIRECT"
  "--cache_size=$CACHE_SIZE"
  "--statistics=0"
  "--disable_auto_compactions=1"
  "--max_background_compactions=1"
  "--max_background_flushes=1"
)

run_case() {
  local mode="$1"
  local case_name="$2"
  local benchmark="$3"
  local threads="$4"
  local value_size="$5"
  local num="$6"
  local reads="$7"
  local use_existing_db="$8"
  local db_dir="$9"
  local wal_dir="${10}"

  local log_rel="logs/${mode}_${case_name}.log"
  local stats_rel="stats/${mode}_${case_name}.kv"
  local log_file="$OUT_DIR/$log_rel"
  local stats_file="$OUT_DIR/$stats_rel"

  local xp_opts=()
  if [[ "$mode" == "sim" ]]; then
    xp_opts+=(
      "--simulate_xp_nvm=1"
      "--simulate_xp_line_bytes=$XP_LINE_BYTES"
      "--simulate_xp_buffer_bytes=$XP_BUFFER_BYTES"
      "--simulate_xp_latency_ns=$XP_LATENCY_NS"
      "--simulate_xp_rpq_depth=$XP_RPQ_DEPTH"
      "--simulate_xp_wpq_depth=$XP_WPQ_DEPTH"
      "--simulate_xp_wpq_submit_ns=$XP_WPQ_SUBMIT_NS"
      "--simulate_xp_prefetch_hit_ns=$XP_PREFETCH_HIT_NS"
      "--simulate_xp_enable_prefetch=$XP_ENABLE_PREFETCH"
      "--simulate_xp_stats_file=$stats_file"
    )
    if [[ -n "$XP_PATH_PREFIX" ]]; then
      xp_opts+=("--simulate_xp_path_prefix=$XP_PATH_PREFIX")
    fi
    if [[ -n "$XP_LEVELS" ]]; then
      xp_opts+=("--simulate_xp_levels=$XP_LEVELS")
    fi
  fi

  local cmd=(
    "$DB_BENCH"
    "--db=$db_dir"
    "--wal_dir=$wal_dir"
    "--benchmarks=$benchmark"
    "--threads=$threads"
    "--value_size=$value_size"
    "--num=$num"
    "--reads=$reads"
    "--use_existing_db=$use_existing_db"
    "${common_opts[@]}"
    "${xp_opts[@]}"
  )

  echo "[run] mode=$mode case=$case_name benchmark=$benchmark threads=$threads value_size=$value_size"
  printf '%q ' "${cmd[@]}" >"$OUT_DIR/logs/${mode}_${case_name}.cmd"
  echo >>"$OUT_DIR/logs/${mode}_${case_name}.cmd"
  "${cmd[@]}" 2>&1 | tee "$log_file"

  if [[ "$mode" != "sim" ]]; then
    : >"$stats_file"
  fi

  echo "$mode,$case_name,$benchmark,$threads,$value_size,$log_rel,$stats_rel" >>"$manifest"
}

IFS=',' read -r -a mode_arr <<<"$MODES"
for mode in "${mode_arr[@]}"; do
  if [[ "$mode" != "sim" && "$mode" != "base" ]]; then
    echo "invalid mode: $mode (supported: sim,base)" >&2
    exit 1
  fi

  mode_db="$DB_ROOT/$mode/db"
  mode_wal="$DB_ROOT/$mode/wal"
  rm -rf "$mode_db" "$mode_wal"
  mkdir -p "$mode_db" "$mode_wal"

  run_case "$mode" "00_seed_fillseq" "fillseq" "$THREADS_FILL" "$FILL_VALUE_SIZE" "$NUM_KEYS" "0" "0" "$mode_db" "$mode_wal"
  run_case "$mode" "01_write_lat_64b" "overwrite" "$THREADS_LAT" "64" "$WRITES_LAT" "0" "1" "$mode_db" "$mode_wal"
  run_case "$mode" "02_write_bw_4k" "overwrite" "$THREADS_BW" "4096" "$WRITES_BW" "0" "1" "$mode_db" "$mode_wal"
  run_case "$mode" "03_read_lat_256b" "readrandom" "$THREADS_LAT" "$FILL_VALUE_SIZE" "$NUM_KEYS" "$READS_LAT" "1" "$mode_db" "$mode_wal"
  run_case "$mode" "04_read_bw_256b" "readrandom" "$THREADS_BW" "$FILL_VALUE_SIZE" "$NUM_KEYS" "$READS_BW" "1" "$mode_db" "$mode_wal"
  run_case "$mode" "05_read_lat_seq_256b" "readseq" "$THREADS_LAT" "$FILL_VALUE_SIZE" "$NUM_KEYS" "$READS_LAT" "1" "$mode_db" "$mode_wal"
  run_case "$mode" "06_read_bw_seq_256b" "readseq" "$THREADS_BW" "$FILL_VALUE_SIZE" "$NUM_KEYS" "$READS_BW" "1" "$mode_db" "$mode_wal"
  run_case "$mode" "07_ewr_stress_64b" "overwrite" "$THREADS_BW" "64" "$WRITES_BW" "0" "1" "$mode_db" "$mode_wal"
done

analyze_cmd=(python3 "$ANALYZE_SCRIPT" --run-dir "$OUT_DIR")
if [[ -n "$PMEM_REFERENCE_CSV" ]]; then
  analyze_cmd+=(--reference-csv "$PMEM_REFERENCE_CSV")
fi
"${analyze_cmd[@]}"

cat <<EOF
done: $OUT_DIR
main outputs:
  - $OUT_DIR/case_manifest.csv
  - $OUT_DIR/analysis/envfs_xp_summary.csv
  - $OUT_DIR/analysis/envfs_xp_report.md
EOF
