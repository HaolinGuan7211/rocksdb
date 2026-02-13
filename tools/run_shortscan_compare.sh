#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB_BENCH="${DB_BENCH:-$ROOT_DIR/build/db_bench}"

if [[ ! -x "$DB_BENCH" ]]; then
  echo "db_bench not found or not executable: $DB_BENCH" >&2
  exit 1
fi

EXP_DATE="${EXP_DATE:-$(date +%Y%m%d)}"
GLOBAL_EXP_ID="${GLOBAL_EXP_ID:-0}"
EXPERIMENT_NAME="${EXPERIMENT_NAME:-shortscan}"
RUN_TIME="${RUN_TIME:-$(date +%H%M%S)}"
RUN_TAG="${RUN_TAG:-${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}_${RUN_TIME}}"
EXPERIMENT_ROOT="${EXPERIMENT_ROOT:-$ROOT_DIR/experiment}"
EXPERIMENT_DIR="${EXPERIMENT_DIR:-$EXPERIMENT_ROOT/${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}}"
OUT_DIR="${OUT_DIR:-$EXPERIMENT_DIR/run_results/$RUN_TAG}"
mkdir -p "$OUT_DIR" "$EXPERIMENT_DIR"

PROFILE="${PROFILE:-smoke}"  # smoke | s | m
THREADS="${THREADS:-16}"
FILL_THREADS="${FILL_THREADS:-$THREADS}"
KEY_SIZE="${KEY_SIZE:-16}"
VALUE_SIZE="${VALUE_SIZE:-256}"
COMPRESSION_TYPE="${COMPRESSION_TYPE:-none}"
USE_DIRECT="${USE_DIRECT:-true}"
SKIP_FILL="${SKIP_FILL:-0}"
ISOLATE_BY_CACHE="${ISOLATE_BY_CACHE:-1}"
EXTRA_DB_BENCH_ARGS="${EXTRA_DB_BENCH_ARGS:-}"
CLEAR_OS_CACHE_BETWEEN_STEPS="${CLEAR_OS_CACHE_BETWEEN_STEPS:-0}"
DROP_CACHE_CMD="${DROP_CACHE_CMD:-}"
AUTO_POST_PROCESS="${AUTO_POST_PROCESS:-1}"
PLOT_SCRIPT="${PLOT_SCRIPT:-$ROOT_DIR/tools/plot_shortscan_results.py}"

MIX_GET_RATIO="${MIX_GET_RATIO:-0.10}"
MIX_PUT_RATIO="${MIX_PUT_RATIO:-0.05}"
MIX_SEEK_RATIO="${MIX_SEEK_RATIO:-0.85}"
MIX_KEY_DIST_A="${MIX_KEY_DIST_A:-0.0016}"
MIX_KEY_DIST_B="${MIX_KEY_DIST_B:--0.71}"
MIX_KEYRANGE_DIST_A="${MIX_KEYRANGE_DIST_A:-14.18}"
MIX_KEYRANGE_DIST_B="${MIX_KEYRANGE_DIST_B:--2.917}"
MIX_KEYRANGE_DIST_C="${MIX_KEYRANGE_DIST_C:-0.0164}"
MIX_KEYRANGE_DIST_D="${MIX_KEYRANGE_DIST_D:--0.08082}"
MIX_KEYRANGE_NUM="${MIX_KEYRANGE_NUM:-32}"
MIX_HOT_KEYRANGE_COUNT="${MIX_HOT_KEYRANGE_COUNT:-0}"
MIX_HOTSET_ENABLE="${MIX_HOTSET_ENABLE:-0}"
MIX_HOTSET_RANGE_PCT="${MIX_HOTSET_RANGE_PCT:-0.03}"
MIX_HOTSET_RANGE_ACCESS_PCT="${MIX_HOTSET_RANGE_ACCESS_PCT:-0.88}"
MIX_HOTSET_RANGE_ZIPF_THETA="${MIX_HOTSET_RANGE_ZIPF_THETA:-1.0}"
MIX_HOTSET_KEY_PCT="${MIX_HOTSET_KEY_PCT:-0.01}"
MIX_HOTSET_KEY_ACCESS_PCT="${MIX_HOTSET_KEY_ACCESS_PCT:-0.80}"
MIX_HOTSET_EVENLY_SPREAD_RANGES="${MIX_HOTSET_EVENLY_SPREAD_RANGES:-1}"
MIX_SHIFT_ENABLE="${MIX_SHIFT_ENABLE:-0}"
MIX_SHIFT_MODE="${MIX_SHIFT_MODE:-step_jump}"
MIX_SHIFT_STAGE_SECONDS="${MIX_SHIFT_STAGE_SECONDS:-300}"
MIX_SHIFT_STRIDE_RANGES="${MIX_SHIFT_STRIDE_RANGES:-1}"
MIX_SHIFT_JUMP_MULTIPLIER="${MIX_SHIFT_JUMP_MULTIPLIER:-4}"
MIX_SHIFT_BASE_START_RANGE="${MIX_SHIFT_BASE_START_RANGE:-0}"
MIX_SHIFT_LOG_STAGE_TRANSITIONS="${MIX_SHIFT_LOG_STAGE_TRANSITIONS:-0}"
MIX_ITER_K="${MIX_ITER_K:-0.08}"
MIX_ITER_SIGMA="${MIX_ITER_SIGMA:-1.75}"
MIX_ITER_THETA="${MIX_ITER_THETA:-0}"

EXPERIMENT_TITLE="${EXPERIMENT_TITLE:-RocksDB shortscan benchmark}"
EXPERIMENT_OBJECTIVE="${EXPERIMENT_OBJECTIVE:-在当前文件格式与读模式下，验证装载与 seek 行为是否符合预期。}"
EXPERIMENT_VARIABLES="${EXPERIMENT_VARIABLES:-cache_size,read_mode,seek_nexts,mix_ratio}"
EXPERIMENT_EXPECTATION="${EXPERIMENT_EXPECTATION:-在 realistic locality 下，更大的 cache 预期可提升或稳定 throughput，并改善 tail latency。}"
EXPECTED_CACHE_TPUT_TREND="${EXPECTED_CACHE_TPUT_TREND:-increase}"
EXPECTED_CACHE_P99_TREND="${EXPECTED_CACHE_P99_TREND:-decrease}"
KEY_LOCALITY_DESC="${KEY_LOCALITY_DESC:-mixgraph+seekrandom locality，参数来源于 result csv 中的 cmd_* 字段}"
ROCKSDB_BUILTIN_OPTIMIZATIONS="${ROCKSDB_BUILTIN_OPTIMIZATIONS:-use_direct_reads=$USE_DIRECT,use_direct_io_for_flush_and_compaction=$USE_DIRECT,compression_type=$COMPRESSION_TYPE}"

BASE_DB_DIR="${BASE_DB_DIR:-/tmp/rocksdb_shortscan_base_db_$RUN_TAG}"
BASE_WAL_DIR="${BASE_WAL_DIR:-/tmp/rocksdb_shortscan_base_wal_$RUN_TAG}"
DB_DIR="${DB_DIR:-/tmp/rocksdb_shortscan_db_$RUN_TAG}"
WAL_DIR="${WAL_DIR:-/tmp/rocksdb_shortscan_wal_$RUN_TAG}"
CASE_ROOT="${CASE_ROOT:-/tmp/rocksdb_shortscan_cases_$RUN_TAG}"
CLEAN_CASE_DIRS="${CLEAN_CASE_DIRS:-1}"

EXTRA_ARGS_ARRAY=()
if [[ -n "$EXTRA_DB_BENCH_ARGS" ]]; then
  # Intentionally split on spaces so callers can pass extra db_bench flags.
  # shellcheck disable=SC2206
  EXTRA_ARGS_ARRAY=($EXTRA_DB_BENCH_ARGS)
fi

BASE_NUM_KEYS=50000000
BASE_REALISTIC_READS=200000000
BASE_STEP200_READS=5000000
BASE_WORST_READS_1=65000000
BASE_WORST_READS_4=20000000
BASE_WORST_READS_20=10000000
BASE_WORST_READS_200=4000000
BASE_WORST_READS_10000=1000000

scale_int() {
  local n="$1"
  local s="$2"
  awk -v n="$n" -v s="$s" 'BEGIN { v = int(n * s); if (v < 1) v = 1; print v }'
}

case "$PROFILE" in
  smoke)
    SCALE="${SCALE:-0.01}"
    CACHE_SIZES="${CACHE_SIZES:-$((1<<30)),$((2<<30)),$((4<<30))}"
    ;;
  s)
    SCALE="${SCALE:-1.0}"
    CACHE_SIZES="${CACHE_SIZES:-$((4<<30)),$((8<<30)),$((16<<30))}"
    ;;
  m)
    SCALE="${SCALE:-3.0}"
    CACHE_SIZES="${CACHE_SIZES:-$((4<<30)),$((8<<30)),$((16<<30))}"
    ;;
  *)
    echo "Unknown PROFILE: $PROFILE (supported: smoke|s|m)" >&2
    exit 1
    ;;
esac

NUM_KEYS="${NUM_KEYS:-$(scale_int "$BASE_NUM_KEYS" "$SCALE")}" 
REALISTIC_READS="${REALISTIC_READS:-$(scale_int "$BASE_REALISTIC_READS" "$SCALE")}" 
STEP200_READS="${STEP200_READS:-$(scale_int "$BASE_STEP200_READS" "$SCALE")}" 
WORST_READS_1="${WORST_READS_1:-$(scale_int "$BASE_WORST_READS_1" "$SCALE")}" 
WORST_READS_4="${WORST_READS_4:-$(scale_int "$BASE_WORST_READS_4" "$SCALE")}" 
WORST_READS_20="${WORST_READS_20:-$(scale_int "$BASE_WORST_READS_20" "$SCALE")}" 
WORST_READS_200="${WORST_READS_200:-$(scale_int "$BASE_WORST_READS_200" "$SCALE")}" 
WORST_READS_10000="${WORST_READS_10000:-$(scale_int "$BASE_WORST_READS_10000" "$SCALE")}" 

mapfile -t CACHE_SIZE_ARRAY < <(echo "$CACHE_SIZES" | tr ',' '\n')

run_step() {
  local name="$1"
  shift
  local log_file="$OUT_DIR/${name}.log"
  echo "[$(date '+%F %T')] START $name" | tee -a "$OUT_DIR/runner.log"
  echo "$DB_BENCH $* ${EXTRA_ARGS_ARRAY[*]}" >"$OUT_DIR/${name}.cmd"
  "$DB_BENCH" "$@" "${EXTRA_ARGS_ARRAY[@]}" 2>&1 | tee "$log_file"
  echo "[$(date '+%F %T')] END   $name" | tee -a "$OUT_DIR/runner.log"
}

drop_os_cache() {
  local reason="$1"
  if [[ "$CLEAR_OS_CACHE_BETWEEN_STEPS" != "1" ]]; then
    return 0
  fi
  if [[ -z "$DROP_CACHE_CMD" ]]; then
    echo "CLEAR_OS_CACHE_BETWEEN_STEPS=1 but DROP_CACHE_CMD is empty" >&2
    exit 1
  fi
  echo "[$(date '+%F %T')] DROP_CACHE before: $reason" | tee -a "$OUT_DIR/runner.log"
  bash -lc "$DROP_CACHE_CMD"
  echo "[$(date '+%F %T')] DROP_CACHE done: $reason" | tee -a "$OUT_DIR/runner.log"
}

human_cache_sizes() {
  local out=()
  local c
  for c in "${CACHE_SIZE_ARRAY[@]}"; do
    out+=("$(awk -v x="$c" 'BEGIN { printf "%.0fGiB", x/(1024*1024*1024) }')")
  done
  local IFS=","
  echo "${out[*]}"
}

write_experiment_plan_doc() {
  local raw_bytes=$((NUM_KEYS * (KEY_SIZE + VALUE_SIZE)))
  local raw_gib
  raw_gib="$(awk -v x="$raw_bytes" 'BEGIN { printf "%.3f", x/(1024*1024*1024) }')"
  local cache_human
  cache_human="$(human_cache_sizes)"
  local plan="$EXPERIMENT_DIR/experiment_plan.md"

  cat >"$plan" <<EOF
# 实验计划

## 身份信息
- exp_date: $EXP_DATE
- global_exp_id: $GLOBAL_EXP_ID
- experiment_name: $EXPERIMENT_NAME
- run_tag: $RUN_TAG
- title: $EXPERIMENT_TITLE

## 目标与预期
- objective: $EXPERIMENT_OBJECTIVE
- expectation: $EXPERIMENT_EXPECTATION
- expected_cache_tput_trend: $EXPECTED_CACHE_TPUT_TREND
- expected_cache_p99_trend: $EXPECTED_CACHE_P99_TREND

## 与结果解读强相关的关键参数
- data_scale: num_keys=$NUM_KEYS, estimated_raw_size_gib=$raw_gib, fill_threads=$FILL_THREADS, workload_threads=$THREADS
- key_shape: key_size=$KEY_SIZE, value_size=$VALUE_SIZE
- key_locality: $KEY_LOCALITY_DESC
- cache_sizes_bytes: $CACHE_SIZES
- cache_sizes_human: $cache_human
- rocksdb_builtin_optimizations: $ROCKSDB_BUILTIN_OPTIMIZATIONS
- experiment_variables: $EXPERIMENT_VARIABLES

## 负载计划
- mixgraph: reads=$REALISTIC_READS per-thread, ratio(get/put/seek)=$MIX_GET_RATIO/$MIX_PUT_RATIO/$MIX_SEEK_RATIO
- mixgraph_locality: key_dist=($MIX_KEY_DIST_A,$MIX_KEY_DIST_B), keyrange_dist=($MIX_KEYRANGE_DIST_A,$MIX_KEYRANGE_DIST_B,$MIX_KEYRANGE_DIST_C,$MIX_KEYRANGE_DIST_D), keyrange_num=$MIX_KEYRANGE_NUM, hot_keyranges=$MIX_HOT_KEYRANGE_COUNT, hotset=(enable=$MIX_HOTSET_ENABLE,range_pct=$MIX_HOTSET_RANGE_PCT,range_access_pct=$MIX_HOTSET_RANGE_ACCESS_PCT,range_zipf_theta=$MIX_HOTSET_RANGE_ZIPF_THETA,key_pct=$MIX_HOTSET_KEY_PCT,key_access_pct=$MIX_HOTSET_KEY_ACCESS_PCT,evenly_spread=$MIX_HOTSET_EVENLY_SPREAD_RANGES), shift=(enable=$MIX_SHIFT_ENABLE,mode=$MIX_SHIFT_MODE,stage_seconds=$MIX_SHIFT_STAGE_SECONDS,stride_ranges=$MIX_SHIFT_STRIDE_RANGES,jump_multiplier=$MIX_SHIFT_JUMP_MULTIPLIER,base_start_range=$MIX_SHIFT_BASE_START_RANGE,log_transitions=$MIX_SHIFT_LOG_STAGE_TRANSITIONS), iter=($MIX_ITER_K,$MIX_ITER_SIGMA,$MIX_ITER_THETA)
- seek200: reads=$STEP200_READS per-thread, seek_nexts=200
- worst_locality: seek_nexts={1,4,20,200,10000}, reads={$WORST_READS_1,$WORST_READS_4,$WORST_READS_20,$WORST_READS_200,$WORST_READS_10000}

## 采集指标
- throughput: ops_per_sec, throughput_mb_s
- latency: micros_per_op, seek_p50/p95/p99/p100
- cache/context: cache_hit_ratio_pct, l0_files_end, cumulative_writes_count, uptime_total_s
EOF
}

clone_db_dir() {
  local src="$1"
  local dst="$2"
  mkdir -p "$dst"
  if cp --help 2>/dev/null | grep -q -- '--reflink'; then
    cp -a --reflink=auto "$src/." "$dst/"
  else
    cp -a "$src/." "$dst/"
  fi
}

print_config() {
  cat <<EOF | tee "$OUT_DIR/config.txt"
RUN_TAG=$RUN_TAG
EXP_DATE=$EXP_DATE
GLOBAL_EXP_ID=$GLOBAL_EXP_ID
EXPERIMENT_NAME=$EXPERIMENT_NAME
RUN_TIME=$RUN_TIME
EXPERIMENT_ROOT=$EXPERIMENT_ROOT
EXPERIMENT_DIR=$EXPERIMENT_DIR
PROFILE=$PROFILE
SCALE=$SCALE
DB_BENCH=$DB_BENCH
OUT_DIR=$OUT_DIR
NUM_KEYS=$NUM_KEYS
REALISTIC_READS=$REALISTIC_READS
STEP200_READS=$STEP200_READS
WORST_READS_1=$WORST_READS_1
WORST_READS_4=$WORST_READS_4
WORST_READS_20=$WORST_READS_20
WORST_READS_200=$WORST_READS_200
WORST_READS_10000=$WORST_READS_10000
CACHE_SIZES=$CACHE_SIZES
THREADS=$THREADS
FILL_THREADS=$FILL_THREADS
KEY_SIZE=$KEY_SIZE
VALUE_SIZE=$VALUE_SIZE
COMPRESSION_TYPE=$COMPRESSION_TYPE
USE_DIRECT=$USE_DIRECT
SKIP_FILL=$SKIP_FILL
ISOLATE_BY_CACHE=$ISOLATE_BY_CACHE
CLEAR_OS_CACHE_BETWEEN_STEPS=$CLEAR_OS_CACHE_BETWEEN_STEPS
DROP_CACHE_CMD=$DROP_CACHE_CMD
AUTO_POST_PROCESS=$AUTO_POST_PROCESS
PLOT_SCRIPT=$PLOT_SCRIPT
MIX_GET_RATIO=$MIX_GET_RATIO
MIX_PUT_RATIO=$MIX_PUT_RATIO
MIX_SEEK_RATIO=$MIX_SEEK_RATIO
MIX_KEY_DIST_A=$MIX_KEY_DIST_A
MIX_KEY_DIST_B=$MIX_KEY_DIST_B
MIX_KEYRANGE_DIST_A=$MIX_KEYRANGE_DIST_A
MIX_KEYRANGE_DIST_B=$MIX_KEYRANGE_DIST_B
MIX_KEYRANGE_DIST_C=$MIX_KEYRANGE_DIST_C
MIX_KEYRANGE_DIST_D=$MIX_KEYRANGE_DIST_D
MIX_KEYRANGE_NUM=$MIX_KEYRANGE_NUM
MIX_HOT_KEYRANGE_COUNT=$MIX_HOT_KEYRANGE_COUNT
MIX_ITER_K=$MIX_ITER_K
MIX_ITER_SIGMA=$MIX_ITER_SIGMA
MIX_ITER_THETA=$MIX_ITER_THETA
EXPERIMENT_TITLE=$EXPERIMENT_TITLE
EXPERIMENT_OBJECTIVE=$EXPERIMENT_OBJECTIVE
EXPERIMENT_VARIABLES=$EXPERIMENT_VARIABLES
EXPERIMENT_EXPECTATION=$EXPERIMENT_EXPECTATION
EXPECTED_CACHE_TPUT_TREND=$EXPECTED_CACHE_TPUT_TREND
EXPECTED_CACHE_P99_TREND=$EXPECTED_CACHE_P99_TREND
KEY_LOCALITY_DESC=$KEY_LOCALITY_DESC
ROCKSDB_BUILTIN_OPTIMIZATIONS=$ROCKSDB_BUILTIN_OPTIMIZATIONS
BASE_DB_DIR=$BASE_DB_DIR
BASE_WAL_DIR=$BASE_WAL_DIR
DB_DIR=$DB_DIR
WAL_DIR=$WAL_DIR
CASE_ROOT=$CASE_ROOT
CLEAN_CASE_DIRS=$CLEAN_CASE_DIRS
EXTRA_DB_BENCH_ARGS=$EXTRA_DB_BENCH_ARGS
EOF
}

print_config
write_experiment_plan_doc

if [[ "$ISOLATE_BY_CACHE" == "1" ]]; then
  mkdir -p "$BASE_DB_DIR" "$BASE_WAL_DIR" "$CASE_ROOT"
else
  mkdir -p "$DB_DIR" "$WAL_DIR"
fi

if [[ "$SKIP_FILL" == "1" ]]; then
  echo "[$(date '+%F %T')] SKIP 01_fillrandom (SKIP_FILL=1)" | tee -a "$OUT_DIR/runner.log"
else
  if [[ "$ISOLATE_BY_CACHE" == "1" ]]; then
    fill_db="$BASE_DB_DIR"
    fill_wal="$BASE_WAL_DIR"
  else
    fill_db="$DB_DIR"
    fill_wal="$WAL_DIR"
  fi

  run_step "01_fillrandom" \
    --db="$fill_db" \
    --wal_dir="$fill_wal" \
    --benchmarks=fillrandom,stats \
    --statistics \
    --num="$NUM_KEYS" \
    --key_size="$KEY_SIZE" \
    --value_size="$VALUE_SIZE" \
    --threads="$FILL_THREADS" \
    --compression_type="$COMPRESSION_TYPE" \
    --cache_size="${CACHE_SIZE_ARRAY[0]}" \
    --use_direct_reads="$USE_DIRECT" \
    --use_direct_io_for_flush_and_compaction="$USE_DIRECT"
fi

for cache_size in "${CACHE_SIZE_ARRAY[@]}"; do
  current_db="$DB_DIR"
  current_wal="$WAL_DIR"

  if [[ "$ISOLATE_BY_CACHE" == "1" ]]; then
    current_db="$CASE_ROOT/cache_${cache_size}_db"
    current_wal="$CASE_ROOT/cache_${cache_size}_wal"

    if [[ -d "$current_db" || -d "$current_wal" ]]; then
      if [[ "$CLEAN_CASE_DIRS" == "1" ]]; then
        rm -rf "$current_db" "$current_wal"
      else
        echo "Case dir exists and CLEAN_CASE_DIRS=0: $current_db" >&2
        exit 1
      fi
    fi

    mkdir -p "$current_wal"
    clone_db_dir "$BASE_DB_DIR" "$current_db"
  fi

  drop_os_cache "02_mixgraph_cache_${cache_size}"
  run_step "02_mixgraph_cache_${cache_size}" \
    --db="$current_db" \
    --wal_dir="$current_wal" \
    --use_existing_db=1 \
    --benchmarks=mixgraph,stats \
    --statistics \
    --num="$NUM_KEYS" \
    --reads="$REALISTIC_READS" \
    --threads="$THREADS" \
    --key_size="$KEY_SIZE" \
    --compression_type="$COMPRESSION_TYPE" \
    --cache_size="$cache_size" \
    --use_direct_reads="$USE_DIRECT" \
    --use_direct_io_for_flush_and_compaction="$USE_DIRECT" \
    --value_k=0.9 --value_sigma=256 --value_theta=0 \
    --key_dist_a="$MIX_KEY_DIST_A" --key_dist_b="$MIX_KEY_DIST_B" \
    --keyrange_dist_a="$MIX_KEYRANGE_DIST_A" --keyrange_dist_b="$MIX_KEYRANGE_DIST_B" \
    --keyrange_dist_c="$MIX_KEYRANGE_DIST_C" --keyrange_dist_d="$MIX_KEYRANGE_DIST_D" \
    --keyrange_num="$MIX_KEYRANGE_NUM" \
    --mix_hot_keyrange_count="$MIX_HOT_KEYRANGE_COUNT" \
    --mix_hotset_enable="$MIX_HOTSET_ENABLE" \
    --mix_hotset_range_pct="$MIX_HOTSET_RANGE_PCT" \
    --mix_hotset_range_access_pct="$MIX_HOTSET_RANGE_ACCESS_PCT" \
    --mix_hotset_range_zipf_theta="$MIX_HOTSET_RANGE_ZIPF_THETA" \
    --mix_hotset_key_pct="$MIX_HOTSET_KEY_PCT" \
    --mix_hotset_key_access_pct="$MIX_HOTSET_KEY_ACCESS_PCT" \
    --mix_hotset_evenly_spread_ranges="$MIX_HOTSET_EVENLY_SPREAD_RANGES" \
    --mix_shift_enable="$MIX_SHIFT_ENABLE" \
    --mix_shift_mode="$MIX_SHIFT_MODE" \
    --mix_shift_stage_seconds="$MIX_SHIFT_STAGE_SECONDS" \
    --mix_shift_stride_ranges="$MIX_SHIFT_STRIDE_RANGES" \
    --mix_shift_jump_multiplier="$MIX_SHIFT_JUMP_MULTIPLIER" \
    --mix_shift_base_start_range="$MIX_SHIFT_BASE_START_RANGE" \
    --mix_shift_log_stage_transitions="$MIX_SHIFT_LOG_STAGE_TRANSITIONS" \
    --iter_k="$MIX_ITER_K" --iter_sigma="$MIX_ITER_SIGMA" --iter_theta="$MIX_ITER_THETA" \
    --mix_get_ratio="$MIX_GET_RATIO" --mix_put_ratio="$MIX_PUT_RATIO" --mix_seek_ratio="$MIX_SEEK_RATIO"
  drop_os_cache "03_seek200_cache_${cache_size}"

  run_step "03_seek200_cache_${cache_size}" \
    --db="$current_db" \
    --wal_dir="$current_wal" \
    --use_existing_db=1 \
    --benchmarks=seekrandom,stats \
    --statistics \
    --num="$NUM_KEYS" \
    --reads="$STEP200_READS" \
    --threads="$THREADS" \
    --key_size="$KEY_SIZE" \
    --compression_type="$COMPRESSION_TYPE" \
    --cache_size="$cache_size" \
    --use_direct_reads="$USE_DIRECT" \
    --use_direct_io_for_flush_and_compaction="$USE_DIRECT" \
    --seek_nexts=200
  drop_os_cache "04_worst_seek1_cache_${cache_size}"

  run_step "04_worst_seek1_cache_${cache_size}" \
    --db="$current_db" --wal_dir="$current_wal" --use_existing_db=1 \
    --benchmarks=seekrandom,stats --statistics \
    --num="$NUM_KEYS" --reads="$WORST_READS_1" --threads="$THREADS" \
    --key_size="$KEY_SIZE" --compression_type="$COMPRESSION_TYPE" --cache_size="$cache_size" \
    --use_direct_reads="$USE_DIRECT" --use_direct_io_for_flush_and_compaction="$USE_DIRECT" \
    --seek_nexts=1
  drop_os_cache "05_worst_seek4_cache_${cache_size}"

  run_step "05_worst_seek4_cache_${cache_size}" \
    --db="$current_db" --wal_dir="$current_wal" --use_existing_db=1 \
    --benchmarks=seekrandom,stats --statistics \
    --num="$NUM_KEYS" --reads="$WORST_READS_4" --threads="$THREADS" \
    --key_size="$KEY_SIZE" --compression_type="$COMPRESSION_TYPE" --cache_size="$cache_size" \
    --use_direct_reads="$USE_DIRECT" --use_direct_io_for_flush_and_compaction="$USE_DIRECT" \
    --seek_nexts=4
  drop_os_cache "06_worst_seek20_cache_${cache_size}"

  run_step "06_worst_seek20_cache_${cache_size}" \
    --db="$current_db" --wal_dir="$current_wal" --use_existing_db=1 \
    --benchmarks=seekrandom,stats --statistics \
    --num="$NUM_KEYS" --reads="$WORST_READS_20" --threads="$THREADS" \
    --key_size="$KEY_SIZE" --compression_type="$COMPRESSION_TYPE" --cache_size="$cache_size" \
    --use_direct_reads="$USE_DIRECT" --use_direct_io_for_flush_and_compaction="$USE_DIRECT" \
    --seek_nexts=20
  drop_os_cache "07_worst_seek200_cache_${cache_size}"

  run_step "07_worst_seek200_cache_${cache_size}" \
    --db="$current_db" --wal_dir="$current_wal" --use_existing_db=1 \
    --benchmarks=seekrandom,stats --statistics \
    --num="$NUM_KEYS" --reads="$WORST_READS_200" --threads="$THREADS" \
    --key_size="$KEY_SIZE" --compression_type="$COMPRESSION_TYPE" --cache_size="$cache_size" \
    --use_direct_reads="$USE_DIRECT" --use_direct_io_for_flush_and_compaction="$USE_DIRECT" \
    --seek_nexts=200
  drop_os_cache "08_worst_seek10000_cache_${cache_size}"

  run_step "08_worst_seek10000_cache_${cache_size}" \
    --db="$current_db" --wal_dir="$current_wal" --use_existing_db=1 \
    --benchmarks=seekrandom,stats --statistics \
    --num="$NUM_KEYS" --reads="$WORST_READS_10000" --threads=8 \
    --key_size="$KEY_SIZE" --compression_type="$COMPRESSION_TYPE" --cache_size="$cache_size" \
    --use_direct_reads="$USE_DIRECT" --use_direct_io_for_flush_and_compaction="$USE_DIRECT" \
    --seek_nexts=10000
  drop_os_cache "09_final_stats_cache_${cache_size}"

  if [[ "$ISOLATE_BY_CACHE" == "1" ]]; then
    run_step "09_final_stats_cache_${cache_size}" \
      --db="$current_db" --wal_dir="$current_wal" --use_existing_db=1 \
      --benchmarks=stats --statistics \
      --compression_type="$COMPRESSION_TYPE"
  fi
done

if [[ "$ISOLATE_BY_CACHE" != "1" ]]; then
  run_step "09_final_stats" \
    --db="$DB_DIR" --wal_dir="$WAL_DIR" --use_existing_db=1 \
    --benchmarks=stats --statistics \
    --compression_type="$COMPRESSION_TYPE"
fi

echo "All steps completed. Logs in: $OUT_DIR"

if [[ "$AUTO_POST_PROCESS" == "1" ]]; then
  if [[ -f "$PLOT_SCRIPT" ]]; then
    python3 "$PLOT_SCRIPT" --run-dir "$OUT_DIR" --out-dir "$EXPERIMENT_DIR/figures" --experiment-dir "$EXPERIMENT_DIR"
  else
    echo "Post-process skipped: plot script not found: $PLOT_SCRIPT" >&2
  fi
fi
