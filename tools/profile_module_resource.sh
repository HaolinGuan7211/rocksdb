#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  tools/profile_module_resource.sh --out-dir DIR [options] -- <command...>
  tools/profile_module_resource.sh --out-dir DIR [options] --duration SEC

Collect system-level resource traces for RocksDB benchmark attribution.

Options:
  --out-dir DIR          Output directory (required)
  --interval SEC         Sampling interval in seconds (default: 1)
  --target-pid PID       Target PID for pidstat/perf attach
  --target-name NAME     Target process name for pidstat -C (default: db_bench)
  --duration SEC         Sampling duration when no command is provided
  --case-label LABEL     Optional case label metadata (e.g. B2)
  --scenario NAME        Optional scenario metadata (e.g. mixgraph)
  --tag TAG              Optional profile tag (default: timestamp)
  -h, --help             Show help

Notes:
  - iostat is always system-wide.
  - perf stat requires a stable target PID; if unavailable, perf is skipped.
  - For matrix-wide profiling, prefer:
      --target-name db_bench -- <matrix runner command>
USAGE
}

OUT_DIR=""
INTERVAL=1
TARGET_PID=""
TARGET_NAME="db_bench"
DURATION=0
CASE_LABEL=""
SCENARIO=""
TAG="$(date +%Y%m%d_%H%M%S)"
PERF_EVENTS_DEFAULT="cycles,instructions,branches,branch-misses,cache-references,cache-misses,L1-dcache-loads,L1-dcache-load-misses,l2_rqsts.references,l2_rqsts.miss,LLC-loads,LLC-load-misses,task-clock,context-switches,cpu-migrations,page-faults"
PERF_EVENTS="${PERF_EVENTS:-$PERF_EVENTS_DEFAULT}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out-dir)
      OUT_DIR="${2:-}"
      shift 2
      ;;
    --interval)
      INTERVAL="${2:-}"
      shift 2
      ;;
    --target-pid)
      TARGET_PID="${2:-}"
      shift 2
      ;;
    --target-name)
      TARGET_NAME="${2:-}"
      shift 2
      ;;
    --duration)
      DURATION="${2:-}"
      shift 2
      ;;
    --case-label)
      CASE_LABEL="${2:-}"
      shift 2
      ;;
    --scenario)
      SCENARIO="${2:-}"
      shift 2
      ;;
    --tag)
      TAG="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    *)
      echo "unknown arg: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ -z "$OUT_DIR" ]]; then
  echo "--out-dir is required" >&2
  usage
  exit 1
fi

if ! [[ "$INTERVAL" =~ ^[0-9]+$ ]] || (( INTERVAL <= 0 )); then
  echo "--interval must be a positive integer" >&2
  exit 1
fi

if ! [[ "$DURATION" =~ ^[0-9]+$ ]] || (( DURATION < 0 )); then
  echo "--duration must be >= 0" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

if (( $# == 0 )) && (( DURATION <= 0 )); then
  echo "provide command after -- or use --duration SEC" >&2
  exit 1
fi

if ! command -v pidstat >/dev/null 2>&1; then
  echo "pidstat not found" >&2
  exit 1
fi
if ! command -v iostat >/dev/null 2>&1; then
  echo "iostat not found" >&2
  exit 1
fi

CMD=()
if (( $# > 0 )); then
  CMD=("$@")
fi

START_TS="$(date +%s)"
START_HUMAN="$(date '+%Y-%m-%d %H:%M:%S')"

CMD_PID=""
CMD_EXIT=""
PIDSTAT_CPU_PID=""
PIDSTAT_MEM_PID=""
PIDSTAT_IO_PID=""
PIDSTAT_SCHED_PID=""
IOSTAT_PID=""
PERF_PID=""
PERF_TARGET_PID="${TARGET_PID}"

cleanup() {
  set +e
  for p in "$PERF_PID" "$PIDSTAT_CPU_PID" "$PIDSTAT_MEM_PID" "$PIDSTAT_IO_PID" "$PIDSTAT_SCHED_PID" "$IOSTAT_PID"; do
    if [[ -n "$p" ]]; then
      kill "$p" >/dev/null 2>&1 || true
    fi
  done
  wait >/dev/null 2>&1 || true

  END_TS="$(date +%s)"
  END_HUMAN="$(date '+%Y-%m-%d %H:%M:%S')"
  ELAPSED="$((END_TS - START_TS))"

  cat > "$OUT_DIR/metadata.env" <<META
PROFILE_TAG=$TAG
START_TIME=$START_HUMAN
END_TIME=$END_HUMAN
ELAPSED_SEC=$ELAPSED
INTERVAL_SEC=$INTERVAL
CASE_LABEL=$CASE_LABEL
SCENARIO=$SCENARIO
TARGET_PID=$TARGET_PID
TARGET_NAME=$TARGET_NAME
COMMAND_PID=$CMD_PID
COMMAND_EXIT_CODE=$CMD_EXIT
PERF_TARGET_PID=$PERF_TARGET_PID
HOSTNAME=$(hostname)
PWD=$(pwd)
META
}
trap cleanup EXIT

if (( ${#CMD[@]} > 0 )); then
  {
    echo "[profile] command: ${CMD[*]}"
    echo "[profile] start: $START_HUMAN"
  } > "$OUT_DIR/command.log"

  set +e
  "${CMD[@]}" >> "$OUT_DIR/command.log" 2>&1 &
  CMD_PID=$!
  set -e

  if [[ -z "$PERF_TARGET_PID" ]]; then
    PERF_TARGET_PID="$CMD_PID"
  fi
fi

if [[ -z "$TARGET_PID" && -z "$TARGET_NAME" ]]; then
  TARGET_NAME="db_bench"
fi

# pidstat collectors
if [[ -n "$TARGET_PID" ]]; then
  pidstat -u -p "$TARGET_PID" "$INTERVAL" > "$OUT_DIR/pidstat_cpu.log" 2> "$OUT_DIR/pidstat_cpu.err" &
  PIDSTAT_CPU_PID=$!
  pidstat -r -p "$TARGET_PID" "$INTERVAL" > "$OUT_DIR/pidstat_mem.log" 2> "$OUT_DIR/pidstat_mem.err" &
  PIDSTAT_MEM_PID=$!
  pidstat -d -p "$TARGET_PID" "$INTERVAL" > "$OUT_DIR/pidstat_io.log" 2> "$OUT_DIR/pidstat_io.err" &
  PIDSTAT_IO_PID=$!
  pidstat -w -p "$TARGET_PID" "$INTERVAL" > "$OUT_DIR/pidstat_sched.log" 2> "$OUT_DIR/pidstat_sched.err" &
  PIDSTAT_SCHED_PID=$!
else
  pidstat -u -C "$TARGET_NAME" "$INTERVAL" > "$OUT_DIR/pidstat_cpu.log" 2> "$OUT_DIR/pidstat_cpu.err" &
  PIDSTAT_CPU_PID=$!
  pidstat -r -C "$TARGET_NAME" "$INTERVAL" > "$OUT_DIR/pidstat_mem.log" 2> "$OUT_DIR/pidstat_mem.err" &
  PIDSTAT_MEM_PID=$!
  pidstat -d -C "$TARGET_NAME" "$INTERVAL" > "$OUT_DIR/pidstat_io.log" 2> "$OUT_DIR/pidstat_io.err" &
  PIDSTAT_IO_PID=$!
  pidstat -w -C "$TARGET_NAME" "$INTERVAL" > "$OUT_DIR/pidstat_sched.log" 2> "$OUT_DIR/pidstat_sched.err" &
  PIDSTAT_SCHED_PID=$!
fi

# system iostat
iostat -y -x -m "$INTERVAL" > "$OUT_DIR/iostat.log" 2> "$OUT_DIR/iostat.err" &
IOSTAT_PID=$!

# perf stat (best-effort)
if command -v perf >/dev/null 2>&1 && [[ -n "$PERF_TARGET_PID" ]]; then
  set +e
  perf stat -I "$((INTERVAL * 1000))" -p "$PERF_TARGET_PID" \
    -e "$PERF_EVENTS" \
    -o "$OUT_DIR/perf_stat.log" 2> "$OUT_DIR/perf_stat.err" &
  PERF_PID=$!
  set -e
else
  echo "perf skipped: no stable target pid" > "$OUT_DIR/perf_stat.err"
fi

if (( ${#CMD[@]} > 0 )); then
  set +e
  wait "$CMD_PID"
  CMD_EXIT=$?
  set -e
else
  sleep "$DURATION"
  CMD_EXIT=0
fi

if [[ "$CMD_EXIT" != "0" ]]; then
  echo "profiled command exited with code $CMD_EXIT" >&2
  exit "$CMD_EXIT"
fi
