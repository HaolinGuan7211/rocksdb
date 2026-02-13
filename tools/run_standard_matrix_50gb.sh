#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="$ROOT_DIR/tools/run_release_50gb_quick.sh"
DB_BENCH="${DB_BENCH:-$ROOT_DIR/build/db_bench}"
PLOT_MATRIX_SCRIPT="${PLOT_MATRIX_SCRIPT:-$ROOT_DIR/tools/plot_matrix_results.py}"
AGGREGATE_RESOURCE_SCRIPT="${AGGREGATE_RESOURCE_SCRIPT:-$ROOT_DIR/tools/aggregate_resource_ledger.py}"
PROFILE_RESOURCE_SCRIPT="${PROFILE_RESOURCE_SCRIPT:-$ROOT_DIR/tools/profile_module_resource.sh}"
PLOT_RESOURCE_LEDGER_SCRIPT="${PLOT_RESOURCE_LEDGER_SCRIPT:-$ROOT_DIR/tools/plot_resource_ledger.py}"
READPATH_TIMELINE_SCRIPT="${READPATH_TIMELINE_SCRIPT:-$ROOT_DIR/tools/analyze_readpath_timeline.py}"
TAIL_PROBE_SCRIPT="${TAIL_PROBE_SCRIPT:-$ROOT_DIR/tools/run_tail_probe_from_matrix.py}"

if [[ ! -x "$RUN_SCRIPT" ]]; then
  echo "missing runner: $RUN_SCRIPT" >&2
  exit 1
fi
if [[ ! -x "$DB_BENCH" ]]; then
  echo "missing db_bench: $DB_BENCH" >&2
  exit 1
fi

EXP_DATE="${EXP_DATE:-$(date +%Y%m%d)}"
EXPERIMENT_ID="${EXPERIMENT_ID:-${START_EXP_ID:-0}}"
MATRIX_NAME="${MATRIX_NAME:-standard_matrix_50gb}"
TARGET_DB_GB="${TARGET_DB_GB:-50}"
MATRIX_TAG="${MATRIX_TAG:-${EXP_DATE}_experiment${EXPERIMENT_ID}_${MATRIX_NAME}}"
EXPERIMENT_ROOT="${EXPERIMENT_ROOT:-$ROOT_DIR/experiment}"
MATRIX_DIR="${MATRIX_DIR:-$EXPERIMENT_ROOT/$MATRIX_TAG}"
REGISTRY_CSV="$MATRIX_DIR/run_registry.csv"
PLAN_MD="$MATRIX_DIR/matrix_plan.md"
INDEX_MD="$MATRIX_DIR/matrix_index.md"
MATRIX_RUNNER_LOG="${MATRIX_RUNNER_LOG:-$MATRIX_DIR/matrix_runner.log}"
APPEND_MATRIX_LOG="${APPEND_MATRIX_LOG:-0}"

KEY_SIZE="${KEY_SIZE:-16}"
VALUE_SIZE="${VALUE_SIZE:-256}"
COMPRESSION_TYPE="${COMPRESSION_TYPE:-none}"
USE_DIRECT="${USE_DIRECT:-true}"
BASE_FILL_THREADS="${BASE_FILL_THREADS:-8}"
BASE_FILL_CACHE_BYTES="${BASE_FILL_CACHE_BYTES:-$((4<<30))}"

PREPARE_BASE_DB="${PREPARE_BASE_DB:-1}"
FORCE_REBUILD_BASE="${FORCE_REBUILD_BASE:-0}"
REUSE_DB_MODE="${REUSE_DB_MODE:-shared}" # shared | clone
BASE_DB_ROOT="${BASE_DB_ROOT:-/tmp/rocksdb_matrix50_base_${MATRIX_TAG}}"
BASE_WAL_ROOT="${BASE_WAL_ROOT:-/tmp/rocksdb_matrix50_base_wal_${MATRIX_TAG}}"

SKIP_IF_EXISTS="${SKIP_IF_EXISTS:-1}"
CLEAR_OS_CACHE_BETWEEN_STEPS="${CLEAR_OS_CACHE_BETWEEN_STEPS:-0}"
DROP_CACHE_BETWEEN_RUNS="${DROP_CACHE_BETWEEN_RUNS:-0}"
DROP_CACHE_CMD="${DROP_CACHE_CMD:-}"
MAX_RUNS="${MAX_RUNS:-0}"
CACHE_SWEEP_GIBS="${CACHE_SWEEP_GIBS:-4,8,16}"
THREAD_SWEEP_LIST="${THREAD_SWEEP_LIST:-8,16,32}"
PHASE_FILTER="${PHASE_FILTER:-all}" # all | cache_sweep | thread_sweep | locality_sweep | comma list
ENABLE_RESOURCE_LEDGER="${ENABLE_RESOURCE_LEDGER:-1}"
ENABLE_RESOURCE_LEDGER_PLOTS="${ENABLE_RESOURCE_LEDGER_PLOTS:-0}"
ENABLE_READPATH_TIMELINE="${ENABLE_READPATH_TIMELINE:-1}"
ENABLE_TAIL_PROBE="${ENABLE_TAIL_PROBE:-1}"
RESOURCE_LEDGER_STRICT="${RESOURCE_LEDGER_STRICT:-0}"
RESOURCE_LEDGER_PROFILE_ROOT="${RESOURCE_LEDGER_PROFILE_ROOT:-}"
TAIL_PROBE_CASES="${TAIL_PROBE_CASES:-}"
TAIL_PROBE_STEP_IDS="${TAIL_PROBE_STEP_IDS:-02}"
TAIL_PROBE_BASELINE_RUNS="${TAIL_PROBE_BASELINE_RUNS:-5}"
TAIL_PROBE_THRESHOLD_MULTIPLIER="${TAIL_PROBE_THRESHOLD_MULTIPLIER:-1.0}"
TAIL_PROBE_THRESHOLD_MIN_US="${TAIL_PROBE_THRESHOLD_MIN_US:-1000}"
TAIL_PROBE_MAX_SAMPLES="${TAIL_PROBE_MAX_SAMPLES:-20000}"
ENABLE_CASE_PROFILE="${ENABLE_CASE_PROFILE:-0}"
CASE_PROFILE_STRICT="${CASE_PROFILE_STRICT:-0}"
CASE_PROFILE_TARGET_NAME="${CASE_PROFILE_TARGET_NAME:-db_bench}"
CASE_PROFILE_ROOT="${CASE_PROFILE_ROOT:-$MATRIX_DIR/profiles/case_level}"

MIXGRAPH_TOTAL_READS="${MIXGRAPH_TOTAL_READS:-12000000}"
SEEK200_TOTAL_READS="${SEEK200_TOTAL_READS:-1000000}"
WORST1_TOTAL_READS="${WORST1_TOTAL_READS:-4000000}"
WORST4_TOTAL_READS="${WORST4_TOTAL_READS:-2000000}"
WORST20_TOTAL_READS="${WORST20_TOTAL_READS:-1000000}"
WORST200_TOTAL_READS="${WORST200_TOTAL_READS:-400000}"
WORST10000_TOTAL_READS="${WORST10000_TOTAL_READS:-30000}"

RESOURCE_LEDGER_STATUS="not_requested"
RESOURCE_LEDGER_OUTPUT_DIR=""
RESOURCE_LEDGER_PROFILE_USED=""
RESOURCE_LEDGER_PLOTS_STATUS="not_requested"
READPATH_TIMELINE_STATUS="not_requested"
TAIL_PROBE_STATUS="not_requested"
TAIL_PROBE_OUTPUT_DIR=""
CASE_PROFILE_ENABLED_RUNTIME=0

is_binary_flag() {
  local x="$1"
  [[ "$x" == "0" || "$x" == "1" ]]
}

log_info() {
  local msg="$1"
  echo "$msg" | tee -a "$MATRIX_RUNNER_LOG"
}

log_warn() {
  local msg="$1"
  echo "$msg" | tee -a "$MATRIX_RUNNER_LOG" >&2
}

run_logged() {
  "$@" 2>&1 | tee -a "$MATRIX_RUNNER_LOG"
}

IFS=',' read -r -a CACHE_SWEEP_GIB_ARRAY <<<"$CACHE_SWEEP_GIBS"
if [[ "${#CACHE_SWEEP_GIB_ARRAY[@]}" -ne 3 ]]; then
  echo "CACHE_SWEEP_GIBS must contain exactly 3 comma-separated integer values (e.g. 1,2,4)." >&2
  exit 1
fi
CACHE_A1="${CACHE_SWEEP_GIB_ARRAY[0]}"
CACHE_A2="${CACHE_SWEEP_GIB_ARRAY[1]}"
CACHE_A3="${CACHE_SWEEP_GIB_ARRAY[2]}"
for c in "$CACHE_A1" "$CACHE_A2" "$CACHE_A3"; do
  if [[ ! "$c" =~ ^[0-9]+$ ]] || (( c <= 0 )); then
    echo "invalid CACHE_SWEEP_GIBS entry: $c (must be positive integer GiB)" >&2
    exit 1
  fi
done

IFS=',' read -r -a THREAD_SWEEP_ARRAY <<<"$THREAD_SWEEP_LIST"
if [[ "${#THREAD_SWEEP_ARRAY[@]}" -lt 1 ]]; then
  echo "THREAD_SWEEP_LIST must contain at least one integer value (e.g. 8,16,32)." >&2
  exit 1
fi
for t in "${THREAD_SWEEP_ARRAY[@]}"; do
  if [[ ! "$t" =~ ^[0-9]+$ ]] || (( t <= 0 )); then
    echo "invalid THREAD_SWEEP_LIST entry: $t (must be positive integer)" >&2
    exit 1
  fi
done

if [[ "$DROP_CACHE_BETWEEN_RUNS" == "1" && -z "$DROP_CACHE_CMD" ]]; then
  echo "DROP_CACHE_BETWEEN_RUNS=1 requires DROP_CACHE_CMD." >&2
  exit 1
fi

for flag_name in \
  APPEND_MATRIX_LOG \
  ENABLE_RESOURCE_LEDGER \
  ENABLE_RESOURCE_LEDGER_PLOTS \
  ENABLE_TAIL_PROBE \
  RESOURCE_LEDGER_STRICT \
  ENABLE_CASE_PROFILE \
  CASE_PROFILE_STRICT; do
  flag_value="${!flag_name}"
  if ! is_binary_flag "$flag_value"; then
    echo "$flag_name must be 0 or 1 (got: $flag_value)" >&2
    exit 1
  fi
done
unset flag_name flag_value

mkdir -p "$MATRIX_DIR"
mkdir -p "$(dirname "$MATRIX_RUNNER_LOG")"
if [[ "$APPEND_MATRIX_LOG" == "1" ]]; then
  touch "$MATRIX_RUNNER_LOG"
else
  : > "$MATRIX_RUNNER_LOG"
fi

if [[ -n "$RESOURCE_LEDGER_PROFILE_ROOT" ]]; then
  if [[ ! -d "$RESOURCE_LEDGER_PROFILE_ROOT" ]]; then
    log_warn "[matrix] resource ledger profile root not found, ignore: $RESOURCE_LEDGER_PROFILE_ROOT"
    RESOURCE_LEDGER_PROFILE_ROOT=""
  else
    RESOURCE_LEDGER_PROFILE_ROOT="$(cd "$RESOURCE_LEDGER_PROFILE_ROOT" && pwd)"
  fi
fi

if [[ "$ENABLE_CASE_PROFILE" == "1" ]]; then
  if [[ ! -x "$PROFILE_RESOURCE_SCRIPT" ]]; then
    if [[ "$CASE_PROFILE_STRICT" == "1" ]]; then
      log_warn "[matrix] missing profile script: $PROFILE_RESOURCE_SCRIPT"
      exit 1
    fi
    log_warn "[matrix] profile disabled: missing script $PROFILE_RESOURCE_SCRIPT"
  elif ! command -v pidstat >/dev/null 2>&1 || ! command -v iostat >/dev/null 2>&1; then
    if [[ "$CASE_PROFILE_STRICT" == "1" ]]; then
      log_warn "[matrix] profile requires pidstat and iostat in PATH"
      exit 1
    fi
    log_warn "[matrix] profile disabled: pidstat/iostat unavailable"
  else
    CASE_PROFILE_ENABLED_RUNTIME=1
    mkdir -p "$CASE_PROFILE_ROOT"
    CASE_PROFILE_ROOT="$(cd "$CASE_PROFILE_ROOT" && pwd)"
  fi
fi

target_bytes="$(awk -v g="$TARGET_DB_GB" 'BEGIN { printf "%.0f", g * 1024 * 1024 * 1024 }')"
per_kv_bytes=$((KEY_SIZE + VALUE_SIZE))
NUM_KEYS=$((target_bytes / per_kv_bytes))

# label|phase|mode|cache_gib|threads|mix_get|mix_put|mix_seek|note
SPECS=(
  "A1|cache_sweep|seekheavy|$CACHE_A1|16|0.20|0.00|0.80|cache 敏感性 baseline"
  "A2|cache_sweep|seekheavy|$CACHE_A2|16|0.20|0.00|0.80|cache 敏感性 baseline"
  "A3|cache_sweep|seekheavy|$CACHE_A3|16|0.20|0.00|0.80|cache 敏感性 baseline"
)
for i in "${!THREAD_SWEEP_ARRAY[@]}"; do
  t="${THREAD_SWEEP_ARRAY[$i]}"
  label="B$((i + 1))"
  SPECS+=("$label|thread_sweep|seekheavy|8|$t|0.20|0.00|0.80|固定 cache/locality 的 thread 敏感性")
done
SPECS+=(
  "C1|locality_sweep|readheavy|8|16|0.75|0.00|0.25|locality/read-mode 敏感性"
  "C2|locality_sweep|balanced|8|16|0.50|0.00|0.50|locality/read-mode 敏感性"
  "C3|locality_sweep|seekheavy|8|16|0.20|0.00|0.80|locality/read-mode 敏感性"
)

phase_enabled() {
  local phase="$1"
  if [[ -z "$PHASE_FILTER" || "$PHASE_FILTER" == "all" ]]; then
    return 0
  fi
  local item
  IFS=',' read -r -a _phase_filters <<<"$PHASE_FILTER"
  for item in "${_phase_filters[@]}"; do
    if [[ "$item" == "$phase" ]]; then
      return 0
    fi
  done
  return 1
}

RUN_SPECS=()
for spec in "${SPECS[@]}"; do
  IFS='|' read -r _label _phase _mode _cache _threads _get _put _seek _note <<<"$spec"
  if phase_enabled "$_phase"; then
    RUN_SPECS+=("$spec")
  fi
done
unset _label _phase _mode _cache _threads _get _put _seek _note

if [[ "${#RUN_SPECS[@]}" -eq 0 ]]; then
  echo "No runnable specs after PHASE_FILTER=$PHASE_FILTER" >&2
  exit 1
fi

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

prepare_base_db() {
  if [[ "$PREPARE_BASE_DB" != "1" ]]; then
    return 0
  fi

  if [[ "$FORCE_REBUILD_BASE" == "1" ]]; then
    rm -rf "$BASE_DB_ROOT" "$BASE_WAL_ROOT"
  fi

  if [[ -f "$BASE_DB_ROOT/db/CURRENT" ]]; then
    log_info "[matrix] reuse prepared base DB: $BASE_DB_ROOT/db"
    return 0
  fi

  rm -rf "$BASE_DB_ROOT" "$BASE_WAL_ROOT"
  mkdir -p "$BASE_DB_ROOT/db" "$BASE_WAL_ROOT/wal"

  log_info "[matrix] prepare base DB once (50GB): num_keys=$NUM_KEYS"
  "$DB_BENCH" \
    --db="$BASE_DB_ROOT/db" \
    --wal_dir="$BASE_WAL_ROOT/wal" \
    --benchmarks=fillrandom,stats \
    --statistics \
    --num="$NUM_KEYS" \
    --key_size="$KEY_SIZE" \
    --value_size="$VALUE_SIZE" \
    --threads="$BASE_FILL_THREADS" \
    --compression_type="$COMPRESSION_TYPE" \
    --cache_size="$BASE_FILL_CACHE_BYTES" \
    --use_direct_reads="$USE_DIRECT" \
    --use_direct_io_for_flush_and_compaction="$USE_DIRECT" \
    2>&1 | tee "$MATRIX_DIR/base_fillrandom.log" | tee -a "$MATRIX_RUNNER_LOG"
}

write_plan_doc() {
  cat >"$PLAN_MD" <<EOF
# 标准变量矩阵（50GB）

- exp_date: $EXP_DATE
- experiment_id: $EXPERIMENT_ID
- matrix_name: $MATRIX_NAME
- matrix_tag: $MATRIX_TAG
- target_db_gb: $TARGET_DB_GB
- phase_filter: $PHASE_FILTER
- objective: 建立可复现 baseline 矩阵，用于 cache/thread/locality 敏感性分析。
- data_load_strategy: 先准备一次 base DB，后续所有矩阵 case 使用 SKIP_FILL=1。
- reuse_db_mode: $REUSE_DB_MODE
- runtime_budget: 在当前 reads 预算下，单次 run 目标约 30 分钟。
- matrix_runner_log: $MATRIX_RUNNER_LOG
- enable_resource_ledger: $ENABLE_RESOURCE_LEDGER
- enable_resource_ledger_plots: $ENABLE_RESOURCE_LEDGER_PLOTS
- enable_readpath_timeline: $ENABLE_READPATH_TIMELINE
- enable_tail_probe: $ENABLE_TAIL_PROBE
- tail_probe_cases: ${TAIL_PROBE_CASES:-auto(A2/B2/C2)}
- tail_probe_step_ids: $TAIL_PROBE_STEP_IDS
- tail_probe_baseline_runs: $TAIL_PROBE_BASELINE_RUNS
- tail_probe_threshold_multiplier: $TAIL_PROBE_THRESHOLD_MULTIPLIER
- tail_probe_threshold_min_us: $TAIL_PROBE_THRESHOLD_MIN_US
- tail_probe_max_samples: $TAIL_PROBE_MAX_SAMPLES
- enable_case_profile: $ENABLE_CASE_PROFILE (runtime=$CASE_PROFILE_ENABLED_RUNTIME)
- case_profile_root: ${CASE_PROFILE_ROOT:-N/A}
- resource_ledger_profile_root: ${RESOURCE_LEDGER_PROFILE_ROOT:-N/A}

## 矩阵维度
EOF

  if phase_enabled "cache_sweep"; then
    cat >>"$PLAN_MD" <<EOF
- phase A (cache_sweep): cache={$CACHE_A1,$CACHE_A2,$CACHE_A3} GiB，固定 threads=16，固定 mix ratio=0.20/0.00/0.80
EOF
  fi
  if phase_enabled "thread_sweep"; then
    local thread_list
    thread_list="$(IFS=','; echo "${THREAD_SWEEP_ARRAY[*]}")"
    cat >>"$PLAN_MD" <<EOF
- phase B (thread_sweep): threads={$thread_list}，固定 cache=8 GiB，固定 mix ratio=0.20/0.00/0.80
EOF
  fi
  if phase_enabled "locality_sweep"; then
    cat >>"$PLAN_MD" <<EOF
- phase C (locality_sweep): mix ratio={readheavy,balanced,seekheavy}，固定 cache=8 GiB，固定 threads=16
EOF
  fi

  cat >>"$PLAN_MD" <<EOF

## 运行表
| case_seq | experiment_id | label | phase | mode | cache_gib | threads | mix_get | mix_put | mix_seek | note |
|---:|---:|---|---|---|---:|---:|---:|---:|---:|---|
EOF

  local i spec label phase mode cache_gib threads mix_get mix_put mix_seek note
  for i in "${!RUN_SPECS[@]}"; do
    if [[ "$MAX_RUNS" != "0" && $((i + 1)) -gt "$MAX_RUNS" ]]; then
      break
    fi
    spec="${RUN_SPECS[$i]}"
    IFS='|' read -r label phase mode cache_gib threads mix_get mix_put mix_seek note <<<"$spec"
    echo "| $((i + 1)) | $EXPERIMENT_ID | $label | $phase | $mode | $cache_gib | $threads | $mix_get | $mix_put | $mix_seek | $note |" >>"$PLAN_MD"
  done
}

write_registry_header() {
  cat >"$REGISTRY_CSV" <<EOF
seq,experiment_id,case_id,label,phase,mode,cache_gib,threads,mix_get,mix_put,mix_seek,reuse_db_mode,experiment_name,experiment_dir,status
EOF
}

append_registry_line() {
  local seq="$1"
  local experiment_id="$2"
  local case_id="$3"
  local label="$4"
  local phase="$5"
  local mode="$6"
  local cache_gib="$7"
  local threads="$8"
  local mix_get="$9"
  local mix_put="${10}"
  local mix_seek="${11}"
  local experiment_name="${12}"
  local experiment_dir="${13}"
  local status="${14}"
  echo "$seq,$experiment_id,$case_id,$label,$phase,$mode,$cache_gib,$threads,$mix_get,$mix_put,$mix_seek,$REUSE_DB_MODE,$experiment_name,$experiment_dir,$status" >>"$REGISTRY_CSV"
}

write_index_doc() {
  {
    echo "# 矩阵索引"
    echo
    echo "- matrix_dir: $MATRIX_DIR"
    echo "- run_registry: $REGISTRY_CSV"
    echo "- plan: $PLAN_MD"
    echo "- base_db_root: $BASE_DB_ROOT"
    echo "- reuse_db_mode: $REUSE_DB_MODE"
    echo "- matrix_runner_log: $MATRIX_RUNNER_LOG"
    echo "- resource_ledger_status: $RESOURCE_LEDGER_STATUS"
    echo "- resource_ledger_plots_status: $RESOURCE_LEDGER_PLOTS_STATUS"
    echo "- readpath_timeline_status: $READPATH_TIMELINE_STATUS"
    echo "- tail_probe_status: $TAIL_PROBE_STATUS"
    if [[ -n "$RESOURCE_LEDGER_OUTPUT_DIR" ]]; then
      echo "- resource_ledger_output: $RESOURCE_LEDGER_OUTPUT_DIR"
    fi
    if [[ -n "$TAIL_PROBE_OUTPUT_DIR" ]]; then
      echo "- tail_probe_output: $TAIL_PROBE_OUTPUT_DIR"
    fi
    if [[ -n "$RESOURCE_LEDGER_PROFILE_USED" ]]; then
      echo "- resource_ledger_profile_root: $RESOURCE_LEDGER_PROFILE_USED"
    fi
    if [[ "$CASE_PROFILE_ENABLED_RUNTIME" == "1" ]]; then
      echo "- case_profile_root: $CASE_PROFILE_ROOT"
    fi
    echo
    echo "## 运行记录"
    echo "| seq | experiment_id | case_id | label | phase | mode | cache_gib | threads | status | experiment_dir |"
    echo "|---:|---:|---|---|---|---|---:|---:|---|---|"
    awk -F',' 'NR>1 { printf("| %s | %s | %s | %s | %s | %s | %s | %s | %s | `%s` |\n",$1,$2,$3,$4,$5,$6,$7,$8,$15,$14) }' "$REGISTRY_CSV"
    echo
    echo "## 说明"
    echo "- 每个 experiment 目录包含："
    echo "  - experiment_plan.md"
    echo "  - experiment_result.md"
    echo "  - figures/"
    echo "  - run_results/"
    if [[ "$ENABLE_RESOURCE_LEDGER" == "1" ]]; then
      echo
      echo "## 资源账本"
      echo "- 账本状态: $RESOURCE_LEDGER_STATUS"
      if [[ -n "$RESOURCE_LEDGER_OUTPUT_DIR" ]]; then
        echo "- 账本目录: \`$RESOURCE_LEDGER_OUTPUT_DIR\`"
      fi
      if [[ -n "$RESOURCE_LEDGER_PROFILE_USED" ]]; then
        echo "- profile 输入: \`$RESOURCE_LEDGER_PROFILE_USED\`"
      fi
      echo "- 关键产物:"
      echo "  - analysis/resource_ledger/resource_ledger.csv"
      echo "  - analysis/resource_ledger/module_impact_matrix.csv"
      echo "  - analysis/resource_ledger/resource_summary.md"
      echo "  - analysis/resource_ledger/validation.json"
      if [[ -f "$MATRIX_DIR/analysis/resource_ledger/readpath_event_samples.csv" ]]; then
        echo "  - analysis/resource_ledger/readpath_event_samples.csv"
        echo "  - analysis/resource_ledger/event_metric_relevance.csv"
        echo "  - analysis/resource_ledger/readpath_timeline_events.csv"
        if [[ -f "$MATRIX_DIR/analysis/resource_ledger/phaseA_readpath_time_stage_stack.png" ]]; then
          echo "  - analysis/resource_ledger/phaseA_readpath_time_stage_stack.png"
        fi
        if [[ -f "$MATRIX_DIR/analysis/resource_ledger/phaseB_readpath_time_stage_stack.png" ]]; then
          echo "  - analysis/resource_ledger/phaseB_readpath_time_stage_stack.png"
        fi
        if [[ -f "$MATRIX_DIR/analysis/resource_ledger/phaseC_readpath_time_stage_stack.png" ]]; then
          echo "  - analysis/resource_ledger/phaseC_readpath_time_stage_stack.png"
        fi
        if [[ -f "$MATRIX_DIR/analysis/resource_ledger/phaseA_readpath_bandwidth_stage_stack.png" ]]; then
          echo "  - analysis/resource_ledger/phaseA_readpath_bandwidth_stage_stack.png"
        fi
        if [[ -f "$MATRIX_DIR/analysis/resource_ledger/phaseB_readpath_bandwidth_stage_stack.png" ]]; then
          echo "  - analysis/resource_ledger/phaseB_readpath_bandwidth_stage_stack.png"
        fi
        if [[ -f "$MATRIX_DIR/analysis/resource_ledger/phaseC_readpath_bandwidth_stage_stack.png" ]]; then
          echo "  - analysis/resource_ledger/phaseC_readpath_bandwidth_stage_stack.png"
        fi
        echo "  - analysis/resource_ledger/event_relevance_heatmap.png"
        echo "  - analysis/resource_ledger/readpath_timeline_report.md"
      fi
      if [[ -f "$MATRIX_DIR/analysis/resource_ledger/tail_probe/tail_threshold_summary.csv" ]]; then
        echo "  - analysis/resource_ledger/tail_probe/tail_threshold_summary.csv"
        echo "  - analysis/resource_ledger/tail_probe/tail_stage_samples.csv"
        echo "  - analysis/resource_ledger/tail_probe/tail_stage_breakdown.csv"
        if [[ -f "$MATRIX_DIR/analysis/resource_ledger/tail_probe/phaseA_tail_seek_stage_stack.png" ]]; then
          echo "  - analysis/resource_ledger/tail_probe/phaseA_tail_seek_stage_stack.png"
        fi
        if [[ -f "$MATRIX_DIR/analysis/resource_ledger/tail_probe/phaseB_tail_seek_stage_stack.png" ]]; then
          echo "  - analysis/resource_ledger/tail_probe/phaseB_tail_seek_stage_stack.png"
        fi
        if [[ -f "$MATRIX_DIR/analysis/resource_ledger/tail_probe/phaseC_tail_seek_stage_stack.png" ]]; then
          echo "  - analysis/resource_ledger/tail_probe/phaseC_tail_seek_stage_stack.png"
        fi
        echo "  - analysis/resource_ledger/tail_probe/tail_probe_report.md"
      fi
    fi
  } >"$INDEX_MD"
}

resource_ledger_profile_root_for_aggregate() {
  if [[ -n "$RESOURCE_LEDGER_PROFILE_ROOT" && -d "$RESOURCE_LEDGER_PROFILE_ROOT" ]]; then
    echo "$RESOURCE_LEDGER_PROFILE_ROOT"
    return 0
  fi
  if [[ "$CASE_PROFILE_ENABLED_RUNTIME" == "1" && -d "$CASE_PROFILE_ROOT" ]]; then
    echo "$CASE_PROFILE_ROOT"
    return 0
  fi
  echo ""
}

generate_resource_ledger() {
  if [[ "$ENABLE_RESOURCE_LEDGER" != "1" ]]; then
    RESOURCE_LEDGER_STATUS="not_requested"
    RESOURCE_LEDGER_PLOTS_STATUS="not_requested"
    READPATH_TIMELINE_STATUS="not_requested"
    return 0
  fi

  if [[ ! -f "$AGGREGATE_RESOURCE_SCRIPT" ]]; then
    RESOURCE_LEDGER_STATUS="missing_aggregator"
    RESOURCE_LEDGER_PLOTS_STATUS="blocked_by_ledger"
    READPATH_TIMELINE_STATUS="blocked_by_ledger"
    local msg="[matrix] resource ledger skipped: missing script $AGGREGATE_RESOURCE_SCRIPT"
    if [[ "$RESOURCE_LEDGER_STRICT" == "1" ]]; then
      log_warn "$msg"
      return 1
    fi
    log_warn "$msg"
    return 0
  fi

  local profile_root
  profile_root="$(resource_ledger_profile_root_for_aggregate)"
  RESOURCE_LEDGER_PROFILE_USED="$profile_root"

  local cmd=(python3 "$AGGREGATE_RESOURCE_SCRIPT" --matrix-dir "$MATRIX_DIR")
  if [[ -n "$profile_root" ]]; then
    cmd+=(--profile-root "$profile_root")
  fi

  log_info "[matrix] generate resource ledger"
  if run_logged "${cmd[@]}"; then
    RESOURCE_LEDGER_STATUS="generated"
    RESOURCE_LEDGER_OUTPUT_DIR="$MATRIX_DIR/analysis/resource_ledger"
    return 0
  fi

  RESOURCE_LEDGER_STATUS="failed"
  RESOURCE_LEDGER_PLOTS_STATUS="blocked_by_ledger"
  READPATH_TIMELINE_STATUS="blocked_by_ledger"
  local err_msg="[matrix] resource ledger generation failed"
  if [[ "$RESOURCE_LEDGER_STRICT" == "1" ]]; then
    log_warn "$err_msg"
    return 1
  fi
  log_warn "$err_msg (continue, RESOURCE_LEDGER_STRICT=0)"
  return 0
}

generate_resource_ledger_plots() {
  if [[ "$ENABLE_RESOURCE_LEDGER_PLOTS" != "1" ]]; then
    RESOURCE_LEDGER_PLOTS_STATUS="not_requested"
    return 0
  fi
  if [[ "$RESOURCE_LEDGER_STATUS" != "generated" ]]; then
    RESOURCE_LEDGER_PLOTS_STATUS="blocked_by_ledger"
    return 0
  fi
  if [[ ! -f "$PLOT_RESOURCE_LEDGER_SCRIPT" ]]; then
    RESOURCE_LEDGER_PLOTS_STATUS="missing_plot_script"
    local msg="[matrix] resource ledger plots skipped: missing script $PLOT_RESOURCE_LEDGER_SCRIPT"
    if [[ "$RESOURCE_LEDGER_STRICT" == "1" ]]; then
      log_warn "$msg"
      return 1
    fi
    log_warn "$msg"
    return 0
  fi

  log_info "[matrix] generate resource-ledger figures"
  if run_logged python3 "$PLOT_RESOURCE_LEDGER_SCRIPT" --matrix-dir "$MATRIX_DIR"; then
    RESOURCE_LEDGER_PLOTS_STATUS="generated"
    return 0
  fi

  RESOURCE_LEDGER_PLOTS_STATUS="failed"
  local msg="[matrix] resource-ledger figure generation failed"
  if [[ "$RESOURCE_LEDGER_STRICT" == "1" ]]; then
    log_warn "$msg"
    return 1
  fi
  log_warn "$msg (continue, RESOURCE_LEDGER_STRICT=0)"
  return 0
}

cleanup_legacy_resource_ledger_plots() {
  if [[ "$RESOURCE_LEDGER_STATUS" != "generated" ]]; then
    return 0
  fi
  local out_dir="$MATRIX_DIR/analysis/resource_ledger"
  if [[ ! -d "$out_dir" ]]; then
    return 0
  fi
  rm -f \
    "$out_dir/performance_signal_table.csv" \
    "$out_dir/phaseA_cache_multimetric_vertical.png" \
    "$out_dir/phaseB_threads_multimetric_vertical.png" \
    "$out_dir/phaseC_mixratio_multimetric_vertical.png" \
    "$out_dir/scenario_score_heatmap.png" \
    "$out_dir/mixgraph_score_ranking.png" \
    "$out_dir/module_read_path_budget.csv" \
    "$out_dir/phaseA_module_pressure_vertical.png" \
    "$out_dir/phaseB_module_pressure_vertical.png" \
    "$out_dir/phaseC_module_pressure_vertical.png" \
    "$out_dir/phaseA_module_signal_heatmap.png" \
    "$out_dir/phaseB_module_signal_heatmap.png" \
    "$out_dir/phaseC_module_signal_heatmap.png" \
    "$out_dir/module_read_path_report.md" \
    "$out_dir/module_resource_breakdown.csv" \
    "$out_dir/module_resource_breakdown_report.md" \
    "$out_dir/performance_signal_report.md"
}

generate_readpath_timeline() {
  if [[ "$ENABLE_READPATH_TIMELINE" != "1" ]]; then
    READPATH_TIMELINE_STATUS="not_requested"
    return 0
  fi
  if [[ "$RESOURCE_LEDGER_STATUS" != "generated" ]]; then
    READPATH_TIMELINE_STATUS="blocked_by_ledger"
    return 0
  fi
  if [[ ! -f "$READPATH_TIMELINE_SCRIPT" ]]; then
    READPATH_TIMELINE_STATUS="missing_timeline_script"
    local msg="[matrix] readpath timeline skipped: missing script $READPATH_TIMELINE_SCRIPT"
    if [[ "$RESOURCE_LEDGER_STRICT" == "1" ]]; then
      log_warn "$msg"
      return 1
    fi
    log_warn "$msg"
    return 0
  fi

  log_info "[matrix] generate readpath timeline analysis"
  if run_logged python3 "$READPATH_TIMELINE_SCRIPT" --matrix-dir "$MATRIX_DIR"; then
    READPATH_TIMELINE_STATUS="generated"
    return 0
  fi

  READPATH_TIMELINE_STATUS="failed"
  local msg="[matrix] readpath timeline generation failed"
  if [[ "$RESOURCE_LEDGER_STRICT" == "1" ]]; then
    log_warn "$msg"
    return 1
  fi
  log_warn "$msg (continue, RESOURCE_LEDGER_STRICT=0)"
  return 0
}

generate_tail_probe() {
  if [[ "$ENABLE_TAIL_PROBE" != "1" ]]; then
    TAIL_PROBE_STATUS="not_requested"
    return 0
  fi
  if [[ ! -f "$TAIL_PROBE_SCRIPT" ]]; then
    TAIL_PROBE_STATUS="missing_tail_probe_script"
    local msg="[matrix] tail probe skipped: missing script $TAIL_PROBE_SCRIPT"
    if [[ "$RESOURCE_LEDGER_STRICT" == "1" ]]; then
      log_warn "$msg"
      return 1
    fi
    log_warn "$msg"
    return 0
  fi

  local cmd=(
    python3 "$TAIL_PROBE_SCRIPT"
    --matrix-dir "$MATRIX_DIR"
    --step-ids "$TAIL_PROBE_STEP_IDS"
    --baseline-runs "$TAIL_PROBE_BASELINE_RUNS"
    --threshold-multiplier "$TAIL_PROBE_THRESHOLD_MULTIPLIER"
    --threshold-min-us "$TAIL_PROBE_THRESHOLD_MIN_US"
    --max-samples "$TAIL_PROBE_MAX_SAMPLES"
  )
  if [[ -n "$TAIL_PROBE_CASES" ]]; then
    cmd+=(--cases "$TAIL_PROBE_CASES")
  fi

  log_info "[matrix] generate tail probe analysis"
  if run_logged "${cmd[@]}"; then
    TAIL_PROBE_STATUS="generated"
    TAIL_PROBE_OUTPUT_DIR="$MATRIX_DIR/analysis/resource_ledger/tail_probe"
    return 0
  fi

  TAIL_PROBE_STATUS="failed"
  local msg="[matrix] tail probe generation failed"
  if [[ "$RESOURCE_LEDGER_STRICT" == "1" ]]; then
    log_warn "$msg"
    return 1
  fi
  log_warn "$msg (continue, RESOURCE_LEDGER_STRICT=0)"
  return 0
}

run_one() {
  local seq="$1"
  local spec="$2"
  local label phase mode cache_gib threads mix_get mix_put mix_seek note
  IFS='|' read -r label phase mode cache_gib threads mix_get mix_put mix_seek note <<<"$spec"

  local case_id="case${seq}"
  local cache_bytes=$((cache_gib << 30))
  local fill_threads="$threads"
  if (( fill_threads > 8 )); then
    fill_threads=8
  fi

  local experiment_name="experiment${EXPERIMENT_ID}_${label}_${phase}_${mode}_c${cache_gib}g_t${threads}"
  local experiment_dir="$MATRIX_DIR/cases/${label}_${phase}_${mode}_c${cache_gib}g_t${threads}"

  local objective expectation vars expected_tput expected_p99
  case "$phase" in
    cache_sweep)
      objective="在固定 thread 数和 locality 下，测量 cache size 敏感性。"
      expectation="在固定 workload 下，更大的 block cache 通常应提升 throughput 并降低 tail latency。"
      vars="cache_size_gib"
      expected_tput="increase"
      expected_p99="decrease"
      ;;
    thread_sweep)
      objective="在固定 cache 和 locality 下，测量 thread 扩展敏感性。"
      expectation="吞吐应在达到饱和前随 threads 增长；饱和后 latency 可能升高。"
      vars="threads"
      expected_tput="unknown"
      expected_p99="unknown"
      ;;
    locality_sweep)
      objective="在固定 cache 和 threads 下，测量 locality/read-mode 敏感性。"
      expectation="相对 read-heavy，seek-heavy ratio 往往会降低 throughput 并恶化 tail latency。"
      vars="mix_ratio(read_mode)"
      expected_tput="unknown"
      expected_p99="unknown"
      ;;
    *)
      objective="矩阵运行。"
      expectation="在受控变量下观察性能差异。"
      vars="phase_variable"
      expected_tput="unknown"
      expected_p99="unknown"
      ;;
  esac

  if [[ "$SKIP_IF_EXISTS" == "1" && -f "$experiment_dir/experiment_result.md" ]]; then
    log_info "[matrix] skip existing: $experiment_dir"
    append_registry_line "$seq" "$EXPERIMENT_ID" "$case_id" "$label" "$phase" "$mode" "$cache_gib" "$threads" \
      "$mix_get" "$mix_put" "$mix_seek" "$experiment_name" "$experiment_dir" "skipped"
    return 0
  fi

  local run_db_root run_wal_root
  if [[ "$REUSE_DB_MODE" == "clone" ]]; then
    run_db_root="/tmp/rocksdb_matrix50_run_${MATRIX_TAG}_${case_id}"
    run_wal_root="/tmp/rocksdb_matrix50_run_wal_${MATRIX_TAG}_${case_id}"
    rm -rf "$run_db_root" "$run_wal_root"
    mkdir -p "$run_db_root/db" "$run_wal_root/wal"
    clone_db_dir "$BASE_DB_ROOT/db" "$run_db_root/db"
  else
    run_db_root="$BASE_DB_ROOT"
    run_wal_root="/tmp/rocksdb_matrix50_shared_wal_${MATRIX_TAG}_${case_id}"
    rm -rf "$run_wal_root"
    mkdir -p "$run_wal_root/wal"
    if [[ "$mix_put" != "0" && "$mix_put" != "0.0" && "$mix_put" != "0.00" ]]; then
      log_warn "[matrix] warning: shared DB with mix_put=$mix_put can cause cross-run interference"
    fi
  fi

  log_info "[matrix] start seq=$seq experiment_id=$EXPERIMENT_ID case_id=$case_id name=$experiment_name"
  local run_cmd=(
    env
    EXP_DATE="$EXP_DATE"
    GLOBAL_EXP_ID="$EXPERIMENT_ID"
    EXPERIMENT_NAME="$experiment_name"
    EXPERIMENT_DIR="$experiment_dir"
    TARGET_DB_GB="$TARGET_DB_GB"
    KEY_SIZE="$KEY_SIZE"
    VALUE_SIZE="$VALUE_SIZE"
    CACHE_SIZES="$cache_bytes"
    THREADS="$threads"
    FILL_THREADS="$fill_threads"
    COMPRESSION_TYPE="$COMPRESSION_TYPE"
    USE_DIRECT="$USE_DIRECT"
    SKIP_FILL=1
    MIX_GET_RATIO="$mix_get"
    MIX_PUT_RATIO="$mix_put"
    MIX_SEEK_RATIO="$mix_seek"
    MIXGRAPH_TOTAL_READS="$MIXGRAPH_TOTAL_READS"
    SEEK200_TOTAL_READS="$SEEK200_TOTAL_READS"
    WORST1_TOTAL_READS="$WORST1_TOTAL_READS"
    WORST4_TOTAL_READS="$WORST4_TOTAL_READS"
    WORST20_TOTAL_READS="$WORST20_TOTAL_READS"
    WORST200_TOTAL_READS="$WORST200_TOTAL_READS"
    WORST10000_TOTAL_READS="$WORST10000_TOTAL_READS"
    CLEAR_OS_CACHE_BETWEEN_STEPS="$CLEAR_OS_CACHE_BETWEEN_STEPS"
    DROP_CACHE_CMD="$DROP_CACHE_CMD"
    DB_ROOT="$run_db_root"
    WAL_ROOT="$run_wal_root"
    EXPERIMENT_TITLE="50GB standard matrix $label ($phase)"
    EXPERIMENT_OBJECTIVE="$objective"
    EXPERIMENT_VARIABLES="$vars"
    EXPERIMENT_EXPECTATION="$expectation"
    EXPECTED_CACHE_TPUT_TREND="$expected_tput"
    EXPECTED_CACHE_P99_TREND="$expected_p99"
    KEY_LOCALITY_DESC="mode=$mode,mix_ratio=$mix_get/$mix_put/$mix_seek with mixgraph+seekrandom distributions"
    ROCKSDB_BUILTIN_OPTIMIZATIONS="use_direct_reads=$USE_DIRECT,use_direct_io_for_flush_and_compaction=$USE_DIRECT,compression_type=$COMPRESSION_TYPE"
    "$RUN_SCRIPT"
  )

  if [[ "$CASE_PROFILE_ENABLED_RUNTIME" == "1" ]]; then
    local case_profile_dir="$CASE_PROFILE_ROOT/$label"
    mkdir -p "$case_profile_dir"
    run_logged "$PROFILE_RESOURCE_SCRIPT" \
      --out-dir "$case_profile_dir" \
      --case-label "$label" \
      --scenario "*" \
      --target-name "$CASE_PROFILE_TARGET_NAME" \
      -- "${run_cmd[@]}"
  else
    run_logged "${run_cmd[@]}"
  fi

  append_registry_line "$seq" "$EXPERIMENT_ID" "$case_id" "$label" "$phase" "$mode" "$cache_gib" "$threads" \
    "$mix_get" "$mix_put" "$mix_seek" "$experiment_name" "$experiment_dir" "done"

  if [[ "$DROP_CACHE_BETWEEN_RUNS" == "1" ]]; then
    log_info "[matrix] drop os cache between runs"
    bash -lc "$DROP_CACHE_CMD"
  fi
}

write_plan_doc
write_registry_header
prepare_base_db

log_info "[matrix] plan: $PLAN_MD"
log_info "[matrix] registry: $REGISTRY_CSV"
if [[ "$CASE_PROFILE_ENABLED_RUNTIME" == "1" ]]; then
  log_info "[matrix] case profile enabled: $CASE_PROFILE_ROOT"
fi

seq=0
for spec in "${RUN_SPECS[@]}"; do
  seq=$((seq + 1))
  if [[ "$MAX_RUNS" != "0" && "$seq" -gt "$MAX_RUNS" ]]; then
    break
  fi
  run_one "$seq" "$spec"
done

if [[ -f "$PLOT_MATRIX_SCRIPT" ]]; then
  log_info "[matrix] generate matrix analysis dashboard"
  run_logged python3 "$PLOT_MATRIX_SCRIPT" --matrix-dir "$MATRIX_DIR"
else
  log_warn "[matrix] skip analysis: plot script not found: $PLOT_MATRIX_SCRIPT"
fi

generate_resource_ledger
generate_resource_ledger_plots
cleanup_legacy_resource_ledger_plots
generate_readpath_timeline
generate_tail_probe
write_index_doc
log_info "[matrix] done: $MATRIX_DIR"
