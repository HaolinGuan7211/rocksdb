#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="$ROOT_DIR/tools/run_shortscan_compare.sh"
DB_BENCH="${DB_BENCH:-$ROOT_DIR/db_bench}"
PLOT_MATRIX_SCRIPT="$ROOT_DIR/tools/plot_matrix_results.py"
AGGREGATE_RESOURCE_SCRIPT="$ROOT_DIR/tools/aggregate_resource_ledger.py"
PLOT_RESOURCE_LEDGER_SCRIPT="$ROOT_DIR/tools/plot_resource_ledger.py"
READPATH_TIMELINE_SCRIPT="$ROOT_DIR/tools/analyze_readpath_timeline.py"
TAIL_PROBE_SCRIPT="$ROOT_DIR/tools/run_tail_probe_from_matrix.py"

if [[ ! -x "$RUN_SCRIPT" ]]; then
  echo "missing runner: $RUN_SCRIPT" >&2
  exit 1
fi
if [[ ! -x "$DB_BENCH" ]]; then
  echo "missing db_bench: $DB_BENCH" >&2
  exit 1
fi

EXP_DATE="${EXP_DATE:-$(date +%Y%m%d)}"
EXPERIMENT_ID="${EXPERIMENT_ID:-21}"
MATRIX_NAME="${MATRIX_NAME:-mixgraph_shifting_v1}"
MATRIX_TAG="${MATRIX_TAG:-${EXP_DATE}_experiment${EXPERIMENT_ID}_${MATRIX_NAME}}"
EXPERIMENT_ROOT="${EXPERIMENT_ROOT:-$ROOT_DIR/experiment}"
MATRIX_DIR="${MATRIX_DIR:-$EXPERIMENT_ROOT/$MATRIX_TAG}"
REGISTRY_CSV="$MATRIX_DIR/run_registry.csv"
PLAN_MD="$MATRIX_DIR/matrix_plan.md"
INDEX_MD="$MATRIX_DIR/matrix_index.md"
MATRIX_LOG="$MATRIX_DIR/matrix_runner.log"

BASE_DB_DIR="${BASE_DB_DIR:-/tmp/rocksdb_sst_ingest_base_20260209_experiment19_base_ingest50gb_envfs_l0l1/db}"
if [[ ! -f "$BASE_DB_DIR/CURRENT" ]]; then
  echo "base db not found: $BASE_DB_DIR" >&2
  exit 1
fi

NUM_KEYS="${NUM_KEYS:-197379011}"
KEY_SIZE="${KEY_SIZE:-16}"
VALUE_SIZE="${VALUE_SIZE:-256}"
COMPRESSION_TYPE="${COMPRESSION_TYPE:-none}"
USE_DIRECT="${USE_DIRECT:-true}"

MIX_GET_RATIO="${MIX_GET_RATIO:-0.20}"
MIX_PUT_RATIO="${MIX_PUT_RATIO:-0.00}"
MIX_SEEK_RATIO="${MIX_SEEK_RATIO:-0.80}"
MIX_KEY_DIST_A="${MIX_KEY_DIST_A:-0}"
MIX_KEY_DIST_B="${MIX_KEY_DIST_B:-0}"
MIX_KEYRANGE_NUM="${MIX_KEYRANGE_NUM:-256}"
MIX_HOT_KEYRANGE_COUNT="${MIX_HOT_KEYRANGE_COUNT:-0}"
MIX_HOTSET_ENABLE="${MIX_HOTSET_ENABLE:-1}"
MIX_HOTSET_RANGE_PCT="${MIX_HOTSET_RANGE_PCT:-0.03}"
MIX_HOTSET_RANGE_ACCESS_PCT="${MIX_HOTSET_RANGE_ACCESS_PCT:-0.88}"
MIX_HOTSET_RANGE_ZIPF_THETA="${MIX_HOTSET_RANGE_ZIPF_THETA:-1.0}"
MIX_HOTSET_KEY_PCT="${MIX_HOTSET_KEY_PCT:-0.01}"
MIX_HOTSET_KEY_ACCESS_PCT="${MIX_HOTSET_KEY_ACCESS_PCT:-0.80}"
MIX_HOTSET_EVENLY_SPREAD_RANGES="${MIX_HOTSET_EVENLY_SPREAD_RANGES:-0}"

MIX_SHIFT_ENABLE="${MIX_SHIFT_ENABLE:-1}"
MIX_SHIFT_STAGE_SECONDS="${MIX_SHIFT_STAGE_SECONDS:-30}"
MIX_SHIFT_STRIDE_RANGES="${MIX_SHIFT_STRIDE_RANGES:-1}"
MIX_SHIFT_JUMP_MULTIPLIER="${MIX_SHIFT_JUMP_MULTIPLIER:-4}"
MIX_SHIFT_BASE_START_RANGE="${MIX_SHIFT_BASE_START_RANGE:-0}"
MIX_SHIFT_LOG_STAGE_TRANSITIONS="${MIX_SHIFT_LOG_STAGE_TRANSITIONS:-1}"

REALISTIC_READS="${REALISTIC_READS:-1500000}"
STEP200_READS="${STEP200_READS:-125000}"
WORST_READS_1="${WORST_READS_1:-500000}"
WORST_READS_4="${WORST_READS_4:-250000}"
WORST_READS_20="${WORST_READS_20:-125000}"
WORST_READS_200="${WORST_READS_200:-50000}"
WORST_READS_10000="${WORST_READS_10000:-3750}"

ENABLE_RESOURCE_LEDGER="${ENABLE_RESOURCE_LEDGER:-1}"
ENABLE_RESOURCE_LEDGER_PLOTS="${ENABLE_RESOURCE_LEDGER_PLOTS:-1}"
ENABLE_READPATH_TIMELINE="${ENABLE_READPATH_TIMELINE:-1}"
ENABLE_TAIL_PROBE="${ENABLE_TAIL_PROBE:-1}"
TAIL_PROBE_STEP_IDS="${TAIL_PROBE_STEP_IDS:-02}"
TAIL_PROBE_BASELINE_RUNS="${TAIL_PROBE_BASELINE_RUNS:-5}"
TAIL_PROBE_THRESHOLD_MULTIPLIER="${TAIL_PROBE_THRESHOLD_MULTIPLIER:-1.0}"
TAIL_PROBE_THRESHOLD_MIN_US="${TAIL_PROBE_THRESHOLD_MIN_US:-1000}"
TAIL_PROBE_MAX_SAMPLES="${TAIL_PROBE_MAX_SAMPLES:-20000}"

EXTRA_DB_BENCH_ARGS="${EXTRA_DB_BENCH_ARGS:---simulate_xp_nvm=1 --simulate_xp_levels=0,1,2,3,4 --simulate_xp_line_bytes=256 --simulate_xp_buffer_bytes=16384 --simulate_xp_latency_ns=300 --simulate_xp_rpq_depth=64 --simulate_xp_wpq_depth=64 --simulate_xp_wpq_submit_ns=100 --simulate_xp_prefetch_hit_ns=120 --simulate_xp_enable_prefetch=true}"

mkdir -p "$MATRIX_DIR/cases" "$MATRIX_DIR/analysis"
: >"$MATRIX_LOG"

log_info() {
  local msg="$1"
  echo "$msg" | tee -a "$MATRIX_LOG"
}

write_registry_header() {
  cat >"$REGISTRY_CSV" <<EOF
seq,experiment_id,case_id,label,phase,mode,cache_gib,threads,mix_get,mix_put,mix_seek,reuse_db_mode,experiment_name,experiment_dir,status
EOF
}

append_registry() {
  local seq="$1"
  local case_id="$2"
  local label="$3"
  local phase="$4"
  local mode="$5"
  local cache_gib="$6"
  local threads="$7"
  local exp_name="$8"
  local exp_dir="$9"
  local status="${10}"
  echo "$seq,$EXPERIMENT_ID,$case_id,$label,$phase,$mode,$cache_gib,$threads,$MIX_GET_RATIO,$MIX_PUT_RATIO,$MIX_SEEK_RATIO,shared,$exp_name,$exp_dir,$status" >>"$REGISTRY_CSV"
}

write_plan_doc() {
  cat >"$PLAN_MD" <<EOF
# MixGraph 热点漂移矩阵（V1）

- matrix_tag: $MATRIX_TAG
- matrix_dir: $MATRIX_DIR
- base_db_dir: $BASE_DB_DIR
- num_keys: $NUM_KEYS
- key_size/value_size: $KEY_SIZE/$VALUE_SIZE
- mix_ratio(get/put/seek): $MIX_GET_RATIO/$MIX_PUT_RATIO/$MIX_SEEK_RATIO
- hotset: range_pct=$MIX_HOTSET_RANGE_PCT, range_access_pct=$MIX_HOTSET_RANGE_ACCESS_PCT, key_pct=$MIX_HOTSET_KEY_PCT, key_access_pct=$MIX_HOTSET_KEY_ACCESS_PCT
- shift_defaults: enable=$MIX_SHIFT_ENABLE, stage_seconds=$MIX_SHIFT_STAGE_SECONDS, stride=$MIX_SHIFT_STRIDE_RANGES, jump_multiplier=$MIX_SHIFT_JUMP_MULTIPLIER

## 运行表
| seq | label | phase | mode | cache_bytes | threads | shift_enable | shift_mode | note |
|---:|---|---|---|---:|---:|---:|---|---|
EOF

  local seq=0
  local spec
  for spec in "${SPECS[@]}"; do
    seq=$((seq + 1))
    IFS='|' read -r label phase mode cache_bytes threads shift_enable shift_mode note <<<"$spec"
    echo "| $seq | $label | $phase | $mode | $cache_bytes | $threads | $shift_enable | $shift_mode | $note |" >>"$PLAN_MD"
  done
}

write_index_doc() {
  {
    echo "# 矩阵索引"
    echo
    echo "- matrix_dir: $MATRIX_DIR"
    echo "- matrix_log: $MATRIX_LOG"
    echo "- run_registry: $REGISTRY_CSV"
    echo "- plan: $PLAN_MD"
    echo
    echo "## 运行记录"
    echo "| seq | label | phase | mode | cache_gib | threads | status | experiment_dir |"
    echo "|---:|---|---|---|---:|---:|---|---|"
    awk -F',' 'NR>1 {printf("| %s | %s | %s | %s | %s | %s | %s | `%s` |\n",$1,$4,$5,$6,$7,$8,$15,$14)}' "$REGISTRY_CSV"
  } >"$INDEX_MD"
}

run_one() {
  local seq="$1"
  local spec="$2"
  IFS='|' read -r label phase mode cache_bytes threads shift_enable shift_mode note <<<"$spec"

  local case_id="case${seq}"
  local cache_gib
  cache_gib="$(awk -v b="$cache_bytes" 'BEGIN { printf "%.3f", b/1024/1024/1024 }')"
  local cache_tag
  cache_tag="$(awk -v b="$cache_bytes" 'BEGIN { if (b % (1024*1024*1024) == 0) printf "%dg", b/(1024*1024*1024); else if (b % (1024*1024) == 0) printf "%dm", b/(1024*1024); else printf "%d", b; }')"

  local exp_name="experiment${EXPERIMENT_ID}_${label}_${phase}_${mode}_c${cache_tag}_t${threads}"
  local exp_dir="$MATRIX_DIR/cases/${label}_${phase}_${mode}_c${cache_tag}_t${threads}"
  local wal_dir="/tmp/rocksdb_${MATRIX_TAG}_${label}_wal"

  log_info "[matrix] start seq=$seq label=$label phase=$phase mode=$mode cache=$cache_bytes threads=$threads shift=$shift_enable/$shift_mode"

  set +e
  env \
    DB_BENCH="$DB_BENCH" \
    EXP_DATE="$EXP_DATE" \
    GLOBAL_EXP_ID="$EXPERIMENT_ID" \
    EXPERIMENT_NAME="$exp_name" \
    EXPERIMENT_DIR="$exp_dir" \
    RUN_TAG="${EXP_DATE}_exp${EXPERIMENT_ID}_${exp_name}_$(date +%H%M%S)" \
    PROFILE=s \
    SCALE=1.0 \
    NUM_KEYS="$NUM_KEYS" \
    REALISTIC_READS="$REALISTIC_READS" \
    STEP200_READS="$STEP200_READS" \
    WORST_READS_1="$WORST_READS_1" \
    WORST_READS_4="$WORST_READS_4" \
    WORST_READS_20="$WORST_READS_20" \
    WORST_READS_200="$WORST_READS_200" \
    WORST_READS_10000="$WORST_READS_10000" \
    CACHE_SIZES="$cache_bytes" \
    THREADS="$threads" \
    FILL_THREADS="$threads" \
    KEY_SIZE="$KEY_SIZE" \
    VALUE_SIZE="$VALUE_SIZE" \
    COMPRESSION_TYPE="$COMPRESSION_TYPE" \
    USE_DIRECT="$USE_DIRECT" \
    SKIP_FILL=1 \
    ISOLATE_BY_CACHE=0 \
    DB_DIR="$BASE_DB_DIR" \
    WAL_DIR="$wal_dir" \
    CLEAN_CASE_DIRS=1 \
    MIX_GET_RATIO="$MIX_GET_RATIO" \
    MIX_PUT_RATIO="$MIX_PUT_RATIO" \
    MIX_SEEK_RATIO="$MIX_SEEK_RATIO" \
    MIX_KEY_DIST_A="$MIX_KEY_DIST_A" \
    MIX_KEY_DIST_B="$MIX_KEY_DIST_B" \
    MIX_KEYRANGE_NUM="$MIX_KEYRANGE_NUM" \
    MIX_HOT_KEYRANGE_COUNT="$MIX_HOT_KEYRANGE_COUNT" \
    MIX_HOTSET_ENABLE="$MIX_HOTSET_ENABLE" \
    MIX_HOTSET_RANGE_PCT="$MIX_HOTSET_RANGE_PCT" \
    MIX_HOTSET_RANGE_ACCESS_PCT="$MIX_HOTSET_RANGE_ACCESS_PCT" \
    MIX_HOTSET_RANGE_ZIPF_THETA="$MIX_HOTSET_RANGE_ZIPF_THETA" \
    MIX_HOTSET_KEY_PCT="$MIX_HOTSET_KEY_PCT" \
    MIX_HOTSET_KEY_ACCESS_PCT="$MIX_HOTSET_KEY_ACCESS_PCT" \
    MIX_HOTSET_EVENLY_SPREAD_RANGES="$MIX_HOTSET_EVENLY_SPREAD_RANGES" \
    MIX_SHIFT_ENABLE="$shift_enable" \
    MIX_SHIFT_MODE="$shift_mode" \
    MIX_SHIFT_STAGE_SECONDS="$MIX_SHIFT_STAGE_SECONDS" \
    MIX_SHIFT_STRIDE_RANGES="$MIX_SHIFT_STRIDE_RANGES" \
    MIX_SHIFT_JUMP_MULTIPLIER="$MIX_SHIFT_JUMP_MULTIPLIER" \
    MIX_SHIFT_BASE_START_RANGE="$MIX_SHIFT_BASE_START_RANGE" \
    MIX_SHIFT_LOG_STAGE_TRANSITIONS="$MIX_SHIFT_LOG_STAGE_TRANSITIONS" \
    EXPERIMENT_TITLE="mixgraph shifting matrix $label" \
    EXPERIMENT_OBJECTIVE="phase=$phase mode=$mode, evaluate cache/thread/shift sensitivity under dynamic hot ranges" \
    EXPERIMENT_VARIABLES="$phase" \
    EXPERIMENT_EXPECTATION="dynamic hot-range shift should produce measurable cache-hit and tail-latency deltas" \
    KEY_LOCALITY_DESC="paper hotset + shift(mode=$shift_mode,stage=${MIX_SHIFT_STAGE_SECONDS}s)" \
    EXTRA_DB_BENCH_ARGS="$EXTRA_DB_BENCH_ARGS" \
    "$RUN_SCRIPT" \
    2>&1 | tee -a "$MATRIX_LOG"
  local ec=${PIPESTATUS[0]}
  set -e

  if [[ "$ec" -eq 0 ]]; then
    append_registry "$seq" "$case_id" "$label" "$phase" "$mode" "$cache_gib" "$threads" "$exp_name" "$exp_dir" "done"
  else
    append_registry "$seq" "$case_id" "$label" "$phase" "$mode" "$cache_gib" "$threads" "$exp_name" "$exp_dir" "failed"
    log_info "[matrix] failed seq=$seq label=$label"
    return "$ec"
  fi
}

generate_analysis() {
  if [[ -f "$PLOT_MATRIX_SCRIPT" ]]; then
    log_info "[matrix] generate matrix dashboard"
    python3 "$PLOT_MATRIX_SCRIPT" --matrix-dir "$MATRIX_DIR" 2>&1 | tee -a "$MATRIX_LOG"
  fi

  if [[ "$ENABLE_RESOURCE_LEDGER" == "1" && -f "$AGGREGATE_RESOURCE_SCRIPT" ]]; then
    log_info "[matrix] aggregate resource ledger"
    python3 "$AGGREGATE_RESOURCE_SCRIPT" --matrix-dir "$MATRIX_DIR" 2>&1 | tee -a "$MATRIX_LOG"
  fi

  if [[ "$ENABLE_RESOURCE_LEDGER_PLOTS" == "1" && -f "$PLOT_RESOURCE_LEDGER_SCRIPT" ]]; then
    log_info "[matrix] plot resource ledger"
    python3 "$PLOT_RESOURCE_LEDGER_SCRIPT" --matrix-dir "$MATRIX_DIR" 2>&1 | tee -a "$MATRIX_LOG"
  fi

  if [[ "$ENABLE_READPATH_TIMELINE" == "1" && -f "$READPATH_TIMELINE_SCRIPT" ]]; then
    log_info "[matrix] generate readpath timeline"
    python3 "$READPATH_TIMELINE_SCRIPT" --matrix-dir "$MATRIX_DIR" 2>&1 | tee -a "$MATRIX_LOG"
  fi

  if [[ "$ENABLE_TAIL_PROBE" == "1" && -f "$TAIL_PROBE_SCRIPT" ]]; then
    log_info "[matrix] generate tail probe"
    python3 "$TAIL_PROBE_SCRIPT" \
      --matrix-dir "$MATRIX_DIR" \
      --step-ids "$TAIL_PROBE_STEP_IDS" \
      --baseline-runs "$TAIL_PROBE_BASELINE_RUNS" \
      --threshold-multiplier "$TAIL_PROBE_THRESHOLD_MULTIPLIER" \
      --threshold-min-us "$TAIL_PROBE_THRESHOLD_MIN_US" \
      --max-samples "$TAIL_PROBE_MAX_SAMPLES" \
      2>&1 | tee -a "$MATRIX_LOG"
  fi
}

SPECS=(
  "A1|cache_sweep|seekheavy|524288000|16|1|step_jump|cache sweep 500MB"
  "A2|cache_sweep|seekheavy|1073741824|16|1|step_jump|cache sweep 1GB"
  "A3|cache_sweep|seekheavy|2147483648|16|1|step_jump|cache sweep 2GB"
  "B1|thread_sweep|seekheavy|1073741824|4|1|step_jump|thread sweep t4"
  "B2|thread_sweep|seekheavy|1073741824|8|1|step_jump|thread sweep t8"
  "B3|thread_sweep|seekheavy|1073741824|16|1|step_jump|thread sweep t16"
  "C1|locality_sweep|step_jump|1073741824|8|1|step_jump|shift mode step_jump"
  "C2|locality_sweep|rolling_window|1073741824|8|1|rolling_window|shift mode rolling_window"
  "C3|locality_sweep|static_hotset|1073741824|8|0|step_jump|shift disabled baseline"
)

write_registry_header
write_plan_doc

seq=0
for spec in "${SPECS[@]}"; do
  seq=$((seq + 1))
  run_one "$seq" "$spec"
done

generate_analysis
write_index_doc
log_info "[matrix] done: $MATRIX_DIR"
