#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  tools/replay_profile_from_matrix_cmds.sh --matrix-dir DIR --profile-root DIR [options]

Replay per-step db_bench .cmd files from matrix case run_results and collect profiles.

Options:
  --matrix-dir DIR      Matrix directory (required)
  --profile-root DIR    Profile output root (required)
  --cases LIST          Case labels, comma-separated (default: B1,B2,B3,B4,B5,B6,B7)
  --steps LIST          Step ids, comma-separated (default: 02,03,04,05,06,07,08)
  --max-runs N          Stop after N step runs (default: 0=all)
  --dry-run             Print commands only, do not execute
  -h, --help            Show help
USAGE
}

MATRIX_DIR=""
PROFILE_ROOT=""
CASES="B1,B2,B3,B4,B5,B6,B7"
STEPS="02,03,04,05,06,07,08"
MAX_RUNS=0
DRY_RUN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --matrix-dir)
      MATRIX_DIR="${2:-}"
      shift 2
      ;;
    --profile-root)
      PROFILE_ROOT="${2:-}"
      shift 2
      ;;
    --cases)
      CASES="${2:-}"
      shift 2
      ;;
    --steps)
      STEPS="${2:-}"
      shift 2
      ;;
    --max-runs)
      MAX_RUNS="${2:-}"
      shift 2
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown arg: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ -z "$MATRIX_DIR" || -z "$PROFILE_ROOT" ]]; then
  echo "--matrix-dir and --profile-root are required" >&2
  usage
  exit 1
fi

if ! [[ "$MAX_RUNS" =~ ^[0-9]+$ ]]; then
  echo "--max-runs must be integer >=0" >&2
  exit 1
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MATRIX_DIR="$(cd "$MATRIX_DIR" && pwd)"
PROFILE_ROOT="$(mkdir -p "$PROFILE_ROOT" && cd "$PROFILE_ROOT" && pwd)"

PROFILE_SCRIPT="$ROOT_DIR/tools/profile_module_resource.sh"
if [[ ! -x "$PROFILE_SCRIPT" ]]; then
  echo "missing executable: $PROFILE_SCRIPT" >&2
  exit 1
fi

step_to_scenario() {
  case "$1" in
    02) echo "mixgraph" ;;
    03) echo "seek200" ;;
    04) echo "worst_seek1" ;;
    05) echo "worst_seek4" ;;
    06) echo "worst_seek20" ;;
    07) echo "worst_seek200" ;;
    08) echo "worst_seek10000" ;;
    *)  echo "unknown" ;;
  esac
}

find_latest_run_dir() {
  local case_dir="$1"
  local run_root="$case_dir/run_results"
  if [[ ! -d "$run_root" ]]; then
    return 1
  fi
  ls -1dt "$run_root"/*/ 2>/dev/null | head -n1 | sed 's#/$##'
}

extract_flag_value() {
  local cmd="$1"
  local key="$2"
  local rest
  rest="${cmd#*${key}=}"
  if [[ "$rest" == "$cmd" ]]; then
    return 1
  fi
  printf '%s\n' "${rest%% *}"
}

IFS=',' read -r -a CASE_ARR <<<"$CASES"
IFS=',' read -r -a STEP_ARR <<<"$STEPS"

mkdir -p "$PROFILE_ROOT"
LOG_FILE="$PROFILE_ROOT/replay.log"
{
  echo "[replay] start $(date '+%F %T')"
  echo "[replay] matrix_dir=$MATRIX_DIR"
  echo "[replay] profile_root=$PROFILE_ROOT"
  echo "[replay] cases=$CASES"
  echo "[replay] steps=$STEPS"
} | tee "$LOG_FILE"

run_count=0
for label in "${CASE_ARR[@]}"; do
  case_dir="$(find "$MATRIX_DIR/cases" -maxdepth 1 -mindepth 1 -type d -name "${label}_*" | head -n1)"
  if [[ -z "$case_dir" ]]; then
    echo "[replay] skip $label: case dir not found" | tee -a "$LOG_FILE"
    continue
  fi

  run_dir="$(find_latest_run_dir "$case_dir" || true)"
  if [[ -z "$run_dir" ]]; then
    echo "[replay] skip $label: run_results not found" | tee -a "$LOG_FILE"
    continue
  fi

  echo "[replay] case=$label run_dir=$run_dir" | tee -a "$LOG_FILE"

  for sid in "${STEP_ARR[@]}"; do
    scenario="$(step_to_scenario "$sid")"
    if [[ "$scenario" == "unknown" ]]; then
      echo "[replay] skip $label step=$sid: unknown scenario" | tee -a "$LOG_FILE"
      continue
    fi

    cmd_file="$(ls "$run_dir"/${sid}_*.cmd 2>/dev/null | head -n1 || true)"
    if [[ -z "$cmd_file" ]]; then
      echo "[replay] skip $label step=$sid: cmd file missing" | tee -a "$LOG_FILE"
      continue
    fi

    cmd="$(tr -d '\n' < "$cmd_file" | sed 's/[[:space:]]\+$//')"
    if [[ -z "$cmd" ]]; then
      echo "[replay] skip $label step=$sid: empty cmd" | tee -a "$LOG_FILE"
      continue
    fi

    wal_dir="$(extract_flag_value "$cmd" "--wal_dir" || true)"
    if [[ -n "$wal_dir" ]]; then
      mkdir -p "$wal_dir"
    fi

    out_dir="$PROFILE_ROOT/$label/$scenario"
    mkdir -p "$out_dir"

    echo "[replay] run case=$label step=$sid scenario=$scenario" | tee -a "$LOG_FILE"
    if [[ "$DRY_RUN" == "1" ]]; then
      echo "[dry-run] $PROFILE_SCRIPT --out-dir $out_dir --case-label $label --scenario $scenario --target-name db_bench -- bash -lc '<cmd>'" | tee -a "$LOG_FILE"
    else
      "$PROFILE_SCRIPT" \
        --out-dir "$out_dir" \
        --case-label "$label" \
        --scenario "$scenario" \
        --target-name db_bench \
        -- bash -lc "$cmd"
    fi

    run_count=$((run_count + 1))
    if (( MAX_RUNS > 0 && run_count >= MAX_RUNS )); then
      echo "[replay] stop at max-runs=$MAX_RUNS" | tee -a "$LOG_FILE"
      echo "[replay] end $(date '+%F %T') runs=$run_count" | tee -a "$LOG_FILE"
      exit 0
    fi
  done

done

echo "[replay] end $(date '+%F %T') runs=$run_count" | tee -a "$LOG_FILE"
