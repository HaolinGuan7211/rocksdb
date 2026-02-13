#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ENVFS_TOOL="${ENVFS_TOOL:-$ROOT_DIR/build_nvm/envfs_profile_tool}"
ANALYZE_SCRIPT="${ANALYZE_SCRIPT:-$ROOT_DIR/tools/nvm_fs/analyze_envfs_profile.py}"

if [[ ! -x "$ENVFS_TOOL" ]]; then
  echo "missing envfs_profile_tool: $ENVFS_TOOL" >&2
  echo "build with: cmake -S . -B build_nvm -DCMAKE_BUILD_TYPE=Release && cmake --build build_nvm --target envfs_profile_tool -j8" >&2
  exit 1
fi
if [[ ! -f "$ANALYZE_SCRIPT" ]]; then
  echo "missing analyzer: $ANALYZE_SCRIPT" >&2
  exit 1
fi

RUN_TAG="${RUN_TAG:-envfs_profile_$(date +%Y%m%d_%H%M%S)}"
OUT_DIR="${OUT_DIR:-/tmp/$RUN_TAG}"
WORK_DIR="${WORK_DIR:-$OUT_DIR/work}"
LOG_DIR="$OUT_DIR/logs"
STATS_DIR="$OUT_DIR/stats"
ANALYSIS_DIR="$OUT_DIR/analysis"
SEED="${SEED:-20260207}"

LAT_READS="${LAT_READS:-4000}"
LAT_WRITES="${LAT_WRITES:-4000}"
EWR_WRITES="${EWR_WRITES:-5000}"
BW_OPS="${BW_OPS:-6000}"
CONC_READS="${CONC_READS:-5000}"
CONC_WRITES="${CONC_WRITES:-5000}"

LAT_READ_VALUE_SIZE="${LAT_READ_VALUE_SIZE:-256}"
LAT_WRITE_VALUE_SIZE="${LAT_WRITE_VALUE_SIZE:-64}"
EWR_VALUE_DEFAULT="${EWR_VALUE_DEFAULT:-64}"
BW_VALUE_SIZE="${BW_VALUE_SIZE:-4096}"
CONC_VALUE_SIZE="${CONC_VALUE_SIZE:-256}"

FILE_SIZE_READ_SMALL="${FILE_SIZE_READ_SMALL:-$((64 * 1024 * 1024))}"
FILE_SIZE_LARGE="${FILE_SIZE_LARGE:-$((256 * 1024 * 1024))}"

BW_THREADS="${BW_THREADS:-1,4,8,16}"
CONC_READ_THREADS="${CONC_READ_THREADS:-1,2,4,8,10,12,16,20,24}"
CONC_WRITE_THREADS="${CONC_WRITE_THREADS:-1,2,4,6,8,12}"
EWR_SIZES="${EWR_SIZES:-64,128,192,256,512,1024,4096}"

XP_LINE_BYTES="${XP_LINE_BYTES:-256}"
XP_BUFFER_BYTES="${XP_BUFFER_BYTES:-16384}"
XP_RPQ_DEPTH="${XP_RPQ_DEPTH:-1024}"
XP_WPQ_DEPTH="${XP_WPQ_DEPTH:-1024}"
XP_ENABLE_PREFETCH="${XP_ENABLE_PREFETCH:-true}"
XP_BYPASS_BASE_IO="${XP_BYPASS_BASE_IO:-true}"
XP_SHARE_BUFFER_BETWEEN_RW="${XP_SHARE_BUFFER_BETWEEN_RW:-true}"
XP_FORCED_TAG_INIT_STAGGER_NS="${XP_FORCED_TAG_INIT_STAGGER_NS:-32}"
DETERMINISTIC_SCHEDULE="${DETERMINISTIC_SCHEDULE:-true}"
DETERMINISTIC_CHUNK_OPS="${DETERMINISTIC_CHUNK_OPS:-1}"
XP_PATH_PREFIX="${XP_PATH_PREFIX:-}"

DRAM_SEQ_NS="${DRAM_SEQ_NS:-81}"
DRAM_RAND_NS="${DRAM_RAND_NS:-101}"

LOCAL_XP_LATENCY_NS="${LOCAL_XP_LATENCY_NS:-212}"
LOCAL_XP_PREFETCH_HIT_NS="${LOCAL_XP_PREFETCH_HIT_NS:-96}"
LOCAL_RPQ_PARALLELISM="${LOCAL_RPQ_PARALLELISM:-16}"
LOCAL_WPQ_PARALLELISM="${LOCAL_WPQ_PARALLELISM:-2}"
LOCAL_READ_LINE_PARALLELISM="${LOCAL_READ_LINE_PARALLELISM:-48}"
LOCAL_WRITE_LINE_PARALLELISM="${LOCAL_WRITE_LINE_PARALLELISM:-6}"
LOCAL_RPQ_ARB_NS="${LOCAL_RPQ_ARB_NS:-100}"

BW_RPQ_PARALLELISM="${BW_RPQ_PARALLELISM:-16}"
BW_WPQ_PARALLELISM="${BW_WPQ_PARALLELISM:-1}"
BW_RPQ_ARB_NS="${BW_RPQ_ARB_NS:-160}"

REMOTE_READ_RATIO="${REMOTE_READ_RATIO:-0.592}"
REMOTE_WRITE_RATIO="${REMOTE_WRITE_RATIO:-0.58}"
REMOTE_RPQ_PARALLELISM="${REMOTE_RPQ_PARALLELISM:-10}"
REMOTE_WPQ_PARALLELISM="${REMOTE_WPQ_PARALLELISM:-2}"
REMOTE_READ_LINE_PARALLELISM="${REMOTE_READ_LINE_PARALLELISM:-16}"
REMOTE_WRITE_LINE_PARALLELISM="${REMOTE_WRITE_LINE_PARALLELISM:-2}"
REMOTE_RPQ_ARB_NS="${REMOTE_RPQ_ARB_NS:-160}"

NTSTORE_WPQ_SUBMIT_NS="${NTSTORE_WPQ_SUBMIT_NS:-90}"
CLWB_WPQ_SUBMIT_NS="${CLWB_WPQ_SUBMIT_NS:-62}"
CONC_WPQ_SUBMIT_NS="${CONC_WPQ_SUBMIT_NS:-300}"

mkdir -p "$OUT_DIR" "$WORK_DIR" "$LOG_DIR" "$STATS_DIR" "$ANALYSIS_DIR"

manifest="$OUT_DIR/case_manifest.csv"
echo "case_id,group,op,pattern,location,mode,benchmark,threads,value_size,num,reads,log_file,stats_file,xp_latency_ns,xp_prefetch_hit_ns,xp_wpq_submit_ns,xp_rpq_arb_ns" >"$manifest"

round_div() {
  python3 - "$1" "$2" <<'PY'
import sys
x = float(sys.argv[1])
y = float(sys.argv[2])
print(int(round(x / y)))
PY
}

REMOTE_READ_XP_LATENCY_NS="${REMOTE_READ_XP_LATENCY_NS:-$(round_div "$LOCAL_XP_LATENCY_NS" "$REMOTE_READ_RATIO")}"
REMOTE_XP_PREFETCH_HIT_NS="${REMOTE_XP_PREFETCH_HIT_NS:-$(round_div "$LOCAL_XP_PREFETCH_HIT_NS" "$REMOTE_READ_RATIO")}"
REMOTE_WRITE_XP_LATENCY_NS="${REMOTE_WRITE_XP_LATENCY_NS:-$(round_div "$LOCAL_XP_LATENCY_NS" "$REMOTE_WRITE_RATIO")}"

split_csv() {
  local input="$1"
  local -n out_ref="$2"
  IFS=',' read -r -a out_ref <<<"$input"
}

run_case() {
  local case_id="$1"
  local group="$2"
  local op="$3"
  local pattern="$4"
  local location="$5"
  local mode="$6"
  local benchmark="$7"
  local threads="$8"
  local value_size="$9"
  local num_ops="${10}"
  local reads="${11}"
  local file_size="${12}"
  local xp_latency_ns="${13}"
  local xp_prefetch_hit_ns="${14}"
  local xp_wpq_submit_ns="${15}"
  local xp_rpq_parallelism="${16}"
  local xp_wpq_parallelism="${17}"
  local xp_read_line_parallelism="${18}"
  local xp_write_line_parallelism="${19}"
  local xp_rpq_arb_ns="${20}"

  local log_rel="logs/${case_id}.log"
  local stats_rel="stats/${case_id}.kv"
  local cmd_rel="logs/${case_id}.cmd"
  local log_file="$OUT_DIR/$log_rel"
  local stats_file="$OUT_DIR/$stats_rel"
  local cmd_file="$OUT_DIR/$cmd_rel"

  local cmd=(
    "$ENVFS_TOOL"
    "--case_id=$case_id"
    "--work_dir=$WORK_DIR"
    "--stats_file=$stats_file"
    "--op=$op"
    "--pattern=$pattern"
    "--threads=$threads"
    "--value_size=$value_size"
    "--num_ops=$num_ops"
    "--file_size=$file_size"
    "--seed=$SEED"
    "--xp_line_bytes=$XP_LINE_BYTES"
    "--xp_buffer_bytes=$XP_BUFFER_BYTES"
    "--xp_latency_ns=$xp_latency_ns"
    "--xp_rpq_depth=$XP_RPQ_DEPTH"
    "--xp_wpq_depth=$XP_WPQ_DEPTH"
    "--xp_rpq_parallelism=$xp_rpq_parallelism"
    "--xp_wpq_parallelism=$xp_wpq_parallelism"
    "--xp_read_line_parallelism=$xp_read_line_parallelism"
    "--xp_write_line_parallelism=$xp_write_line_parallelism"
    "--xp_rpq_arb_ns=$xp_rpq_arb_ns"
    "--xp_wpq_submit_ns=$xp_wpq_submit_ns"
    "--xp_prefetch_hit_ns=$xp_prefetch_hit_ns"
    "--xp_dram_seq_read_ns=$DRAM_SEQ_NS"
    "--xp_dram_rand_read_ns=$DRAM_RAND_NS"
    "--xp_enable_prefetch=$XP_ENABLE_PREFETCH"
    "--xp_share_buffer_between_rw=$XP_SHARE_BUFFER_BETWEEN_RW"
    "--xp_bypass_base_io=$XP_BYPASS_BASE_IO"
    "--xp_forced_tag_init_stagger_ns=$XP_FORCED_TAG_INIT_STAGGER_NS"
    "--deterministic_schedule=$DETERMINISTIC_SCHEDULE"
    "--deterministic_chunk_ops=$DETERMINISTIC_CHUNK_OPS"
  )
  if [[ -n "$XP_PATH_PREFIX" ]]; then
    cmd+=("--xp_path_prefix=$XP_PATH_PREFIX")
  fi

  echo "[run] $case_id op=$op pattern=$pattern threads=$threads v=$value_size num_ops=$num_ops"
  printf '%q ' "${cmd[@]}" >"$cmd_file"
  echo >>"$cmd_file"
  "${cmd[@]}" 2>&1 | tee "$log_file"

  echo "$case_id,$group,$op,$pattern,$location,$mode,$benchmark,$threads,$value_size,$num_ops,$reads,$log_rel,$stats_rel,$xp_latency_ns,$xp_prefetch_hit_ns,$xp_wpq_submit_ns,$xp_rpq_arb_ns" >>"$manifest"
}

split_csv "$BW_THREADS" bw_threads_arr
split_csv "$CONC_READ_THREADS" conc_read_threads_arr
split_csv "$CONC_WRITE_THREADS" conc_write_threads_arr
split_csv "$EWR_SIZES" ewr_sizes_arr

run_case "lat_read_seq_256_local" "latency" "read" "seq" "local" "ntstore" "envfs_read_seq" \
  1 "$LAT_READ_VALUE_SIZE" "$LAT_READS" "$LAT_READS" "$FILE_SIZE_READ_SMALL" \
  "$LOCAL_XP_LATENCY_NS" "$LOCAL_XP_PREFETCH_HIT_NS" "$NTSTORE_WPQ_SUBMIT_NS" \
  "$LOCAL_RPQ_PARALLELISM" "$LOCAL_WPQ_PARALLELISM" "$LOCAL_READ_LINE_PARALLELISM" "$LOCAL_WRITE_LINE_PARALLELISM" "$LOCAL_RPQ_ARB_NS"
run_case "lat_read_rand_256_local" "latency" "read" "rand" "local" "ntstore" "envfs_read_rand" \
  1 "$LAT_READ_VALUE_SIZE" "$LAT_READS" "$LAT_READS" "$FILE_SIZE_READ_SMALL" \
  "$LOCAL_XP_LATENCY_NS" "$LOCAL_XP_PREFETCH_HIT_NS" "$NTSTORE_WPQ_SUBMIT_NS" \
  "$LOCAL_RPQ_PARALLELISM" "$LOCAL_WPQ_PARALLELISM" "$LOCAL_READ_LINE_PARALLELISM" "$LOCAL_WRITE_LINE_PARALLELISM" "$LOCAL_RPQ_ARB_NS"
run_case "lat_write_rand_64_nt_local" "latency" "write" "rand" "local" "ntstore" "envfs_write_rand" \
  1 "$LAT_WRITE_VALUE_SIZE" "$LAT_WRITES" 0 "$FILE_SIZE_LARGE" \
  "$LOCAL_XP_LATENCY_NS" "$LOCAL_XP_PREFETCH_HIT_NS" "$NTSTORE_WPQ_SUBMIT_NS" \
  "$LOCAL_RPQ_PARALLELISM" "$LOCAL_WPQ_PARALLELISM" "$LOCAL_READ_LINE_PARALLELISM" "$LOCAL_WRITE_LINE_PARALLELISM" "$LOCAL_RPQ_ARB_NS"
run_case "lat_write_rand_64_clwb_local" "latency" "write" "rand" "local" "store_clwb" "envfs_write_rand" \
  1 "$LAT_WRITE_VALUE_SIZE" "$LAT_WRITES" 0 "$FILE_SIZE_LARGE" \
  "$LOCAL_XP_LATENCY_NS" "$LOCAL_XP_PREFETCH_HIT_NS" "$CLWB_WPQ_SUBMIT_NS" \
  "$LOCAL_RPQ_PARALLELISM" "$LOCAL_WPQ_PARALLELISM" "$LOCAL_READ_LINE_PARALLELISM" "$LOCAL_WRITE_LINE_PARALLELISM" "$LOCAL_RPQ_ARB_NS"

for vs in "${ewr_sizes_arr[@]}"; do
  run_case "ewr_rand_v${vs}_local" "ewr_curve" "write" "rand" "local" "ntstore" "envfs_write_rand" \
    1 "$vs" "$EWR_WRITES" 0 "$FILE_SIZE_LARGE" \
    "$LOCAL_XP_LATENCY_NS" "$LOCAL_XP_PREFETCH_HIT_NS" "$NTSTORE_WPQ_SUBMIT_NS" \
    "$LOCAL_RPQ_PARALLELISM" "$LOCAL_WPQ_PARALLELISM" "$LOCAL_READ_LINE_PARALLELISM" "$LOCAL_WRITE_LINE_PARALLELISM" "$LOCAL_RPQ_ARB_NS"
done

for t in "${bw_threads_arr[@]}"; do
  run_case "bw_read_seq_t${t}_local" "bandwidth" "read" "seq" "local" "ntstore" "envfs_read_seq" \
    "$t" "$BW_VALUE_SIZE" "$BW_OPS" "$BW_OPS" "$FILE_SIZE_LARGE" \
    "$LOCAL_XP_LATENCY_NS" "$LOCAL_XP_PREFETCH_HIT_NS" "$NTSTORE_WPQ_SUBMIT_NS" \
    "$BW_RPQ_PARALLELISM" "$BW_WPQ_PARALLELISM" "$LOCAL_READ_LINE_PARALLELISM" "$LOCAL_WRITE_LINE_PARALLELISM" "$BW_RPQ_ARB_NS"
  run_case "bw_read_rand_t${t}_local" "bandwidth" "read" "rand" "local" "ntstore" "envfs_read_rand" \
    "$t" "$BW_VALUE_SIZE" "$BW_OPS" "$BW_OPS" "$FILE_SIZE_LARGE" \
    "$LOCAL_XP_LATENCY_NS" "$LOCAL_XP_PREFETCH_HIT_NS" "$NTSTORE_WPQ_SUBMIT_NS" \
    "$BW_RPQ_PARALLELISM" "$BW_WPQ_PARALLELISM" "$LOCAL_READ_LINE_PARALLELISM" "$LOCAL_WRITE_LINE_PARALLELISM" "$BW_RPQ_ARB_NS"
  run_case "bw_write_seq_t${t}_local" "bandwidth" "write" "seq" "local" "ntstore" "envfs_write_seq" \
    "$t" "$BW_VALUE_SIZE" "$BW_OPS" 0 "$FILE_SIZE_LARGE" \
    "$LOCAL_XP_LATENCY_NS" "$LOCAL_XP_PREFETCH_HIT_NS" "$NTSTORE_WPQ_SUBMIT_NS" \
    "$BW_RPQ_PARALLELISM" "$BW_WPQ_PARALLELISM" "$LOCAL_READ_LINE_PARALLELISM" "$LOCAL_WRITE_LINE_PARALLELISM" "$BW_RPQ_ARB_NS"
  run_case "bw_write_rand_t${t}_local" "bandwidth" "write" "rand" "local" "ntstore" "envfs_write_rand" \
    "$t" "$BW_VALUE_SIZE" "$BW_OPS" 0 "$FILE_SIZE_LARGE" \
    "$LOCAL_XP_LATENCY_NS" "$LOCAL_XP_PREFETCH_HIT_NS" "$NTSTORE_WPQ_SUBMIT_NS" \
    "$BW_RPQ_PARALLELISM" "$BW_WPQ_PARALLELISM" "$LOCAL_READ_LINE_PARALLELISM" "$LOCAL_WRITE_LINE_PARALLELISM" "$BW_RPQ_ARB_NS"
done

for t in "${conc_read_threads_arr[@]}"; do
  run_case "conc_read_local_t${t}" "concurrency" "read" "rand" "local" "ntstore" "envfs_read_rand" \
    "$t" "$CONC_VALUE_SIZE" "$CONC_READS" "$CONC_READS" "$FILE_SIZE_READ_SMALL" \
    "$LOCAL_XP_LATENCY_NS" "$LOCAL_XP_PREFETCH_HIT_NS" "$NTSTORE_WPQ_SUBMIT_NS" \
    "$LOCAL_RPQ_PARALLELISM" "$LOCAL_WPQ_PARALLELISM" "$LOCAL_READ_LINE_PARALLELISM" "$LOCAL_WRITE_LINE_PARALLELISM" "$LOCAL_RPQ_ARB_NS"
  run_case "conc_read_remote_t${t}" "concurrency" "read" "rand" "remote" "ntstore" "envfs_read_rand" \
    "$t" "$CONC_VALUE_SIZE" "$CONC_READS" "$CONC_READS" "$FILE_SIZE_READ_SMALL" \
    "$REMOTE_READ_XP_LATENCY_NS" "$REMOTE_XP_PREFETCH_HIT_NS" "$NTSTORE_WPQ_SUBMIT_NS" \
    "$REMOTE_RPQ_PARALLELISM" "$REMOTE_WPQ_PARALLELISM" "$REMOTE_READ_LINE_PARALLELISM" "$REMOTE_WRITE_LINE_PARALLELISM" "$REMOTE_RPQ_ARB_NS"
done

for t in "${conc_write_threads_arr[@]}"; do
  run_case "conc_write_local_t${t}" "concurrency" "write" "rand" "local" "ntstore" "envfs_write_rand" \
    "$t" "$CONC_VALUE_SIZE" "$CONC_WRITES" 0 "$FILE_SIZE_LARGE" \
    "$LOCAL_XP_LATENCY_NS" "$LOCAL_XP_PREFETCH_HIT_NS" "$CONC_WPQ_SUBMIT_NS" \
    "$LOCAL_RPQ_PARALLELISM" "$LOCAL_WPQ_PARALLELISM" "$LOCAL_READ_LINE_PARALLELISM" "$LOCAL_WRITE_LINE_PARALLELISM" "$LOCAL_RPQ_ARB_NS"
  run_case "conc_write_remote_t${t}" "concurrency" "write" "rand" "remote" "ntstore" "envfs_write_rand" \
    "$t" "$CONC_VALUE_SIZE" "$CONC_WRITES" 0 "$FILE_SIZE_LARGE" \
    "$REMOTE_WRITE_XP_LATENCY_NS" "$REMOTE_XP_PREFETCH_HIT_NS" "$CONC_WPQ_SUBMIT_NS" \
    "$REMOTE_RPQ_PARALLELISM" "$REMOTE_WPQ_PARALLELISM" "$REMOTE_READ_LINE_PARALLELISM" "$REMOTE_WRITE_LINE_PARALLELISM" "$REMOTE_RPQ_ARB_NS"
done

python3 "$ANALYZE_SCRIPT" --run-dir "$OUT_DIR"

echo "done: $OUT_DIR"
echo "outputs:"
echo "  - $OUT_DIR/case_manifest.csv"
echo "  - $OUT_DIR/analysis/envfs_profile_summary.csv"
echo "  - $OUT_DIR/analysis/envfs_profile_anchors.csv"
echo "  - $OUT_DIR/analysis/envfs_profile_report.md"
