#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

MATRIX_DIR=""
ONCE=0
REFRESH_SEC="${REFRESH_SEC:-5}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --matrix-dir)
      MATRIX_DIR="${2:-}"
      shift 2
      ;;
    --refresh)
      REFRESH_SEC="${2:-5}"
      shift 2
      ;;
    --once)
      ONCE=1
      shift
      ;;
    *)
      if [[ -z "$MATRIX_DIR" ]]; then
        MATRIX_DIR="$1"
        shift
      else
        echo "Unknown argument: $1" >&2
        exit 1
      fi
      ;;
  esac
done

if [[ -z "$MATRIX_DIR" ]]; then
  MATRIX_DIR="$(ls -dt "$ROOT_DIR"/experiment/*_standard_matrix_50gb_exp* 2>/dev/null | head -n1 || true)"
fi

if [[ -z "$MATRIX_DIR" || ! -d "$MATRIX_DIR" ]]; then
  echo "Matrix dir not found. Use --matrix-dir <path>." >&2
  exit 1
fi

RUNNER_LOG="$MATRIX_DIR/matrix_runner.log"
PLAN_MD="$MATRIX_DIR/matrix_plan.md"
REGISTRY_CSV="$MATRIX_DIR/run_registry.csv"

if [[ ! -f "$RUNNER_LOG" ]]; then
  echo "Missing runner log: $RUNNER_LOG" >&2
  exit 1
fi

human_duration() {
  local s="${1:-0}"
  if [[ -z "$s" || "$s" -lt 0 ]]; then
    echo "NA"
    return
  fi
  local h=$((s / 3600))
  local m=$(((s % 3600) / 60))
  local sec=$((s % 60))
  printf "%02d:%02d:%02d" "$h" "$m" "$sec"
}

last_finished_ops() {
  tail -n 8000 "$RUNNER_LOG" 2>/dev/null | grep -Eo 'finished [0-9]+ ops' | tail -n1 | awk '{print $2}'
}

total_runs() {
  if [[ -f "$PLAN_MD" ]]; then
    grep -Ec '^\| [0-9]+ \|' "$PLAN_MD" || true
  else
    echo "0"
  fi
}

done_runs() {
  if [[ -f "$REGISTRY_CSV" ]]; then
    awk -F',' 'NR>1 && $NF=="done"{c++} END{print c+0}' "$REGISTRY_CSV"
  else
    echo "0"
  fi
}

skipped_runs() {
  if [[ -f "$REGISTRY_CSV" ]]; then
    awk -F',' 'NR>1 && $NF=="skipped"{c++} END{print c+0}' "$REGISTRY_CSV"
  else
    echo "0"
  fi
}

last_matrix_start() {
  grep -E '\[matrix\] start seq=' "$RUNNER_LOG" | tail -n1 || true
}

parse_field() {
  local text="$1"
  local key="$2"
  echo "$text" | sed -n "s/.*$key=\\([^ ]*\\).*/\\1/p"
}

render_once() {
  local now
  now="$(date '+%F %T')"

  local total done skip
  total="$(total_runs)"
  done="$(done_runs)"
  skip="$(skipped_runs)"

  local last_start seq exp_id exp_name
  last_start="$(last_matrix_start)"
  seq="$(parse_field "$last_start" "seq")"
  exp_id="$(parse_field "$last_start" "exp_id")"
  exp_name="$(parse_field "$last_start" "name")"

  local db_line db_pid bench num reads threads seek_nexts
  db_line="$(pgrep -fa 'db_bench' | grep -v 'watch_matrix_progress' | head -n1 || true)"
  db_pid="$(echo "$db_line" | awk '{print $1}')"
  bench="$(parse_field "$db_line" "--benchmarks")"
  num="$(parse_field "$db_line" "--num")"
  reads="$(parse_field "$db_line" "--reads")"
  threads="$(parse_field "$db_line" "--threads")"
  seek_nexts="$(parse_field "$db_line" "--seek_nexts")"

  local progress target target_note elapsed pct eta
  progress="$(last_finished_ops)"
  target=0
  target_note=""
  if [[ -n "$bench" ]]; then
    if [[ "$bench" == "fillrandom,stats" ]]; then
      target="${num:-0}"
    elif [[ "$bench" == "mixgraph,stats" || "$bench" == "seekrandom,stats" ]]; then
      if [[ -n "$reads" && -n "$threads" ]]; then
        target=$((reads * threads))
        target_note="(reads*threads)"
      elif [[ -n "$reads" ]]; then
        target="$reads"
      fi
    fi
  fi

  elapsed="0"
  if [[ -n "$db_pid" ]]; then
    elapsed="$(ps -p "$db_pid" -o etimes= | tr -d ' ' || echo 0)"
  fi

  pct="NA"
  eta="NA"
  if [[ "$target" -gt 0 && -n "$progress" && "$progress" -gt 0 ]]; then
    pct="$(awk -v p="$progress" -v t="$target" 'BEGIN { printf "%.2f", (p*100.0)/t }')"
    eta="$(awk -v p="$progress" -v t="$target" -v e="$elapsed" 'BEGIN { if (p>0) printf "%.0f", e*(t-p)/p; else print "-1" }')"
  fi

  if [[ -t 1 ]]; then
    printf '\033[2J\033[H'
  fi

  echo "Matrix Progress Watch"
  echo "time: $now"
  echo "matrix_dir: $MATRIX_DIR"
  echo
  echo "run_status: done=$done skipped=$skip total=$total"
  if [[ -n "$seq" ]]; then
    echo "current_case: seq=$seq exp_id=${exp_id:-NA} name=${exp_name:-NA}"
  elif [[ "$bench" == "fillrandom,stats" && "$db_line" == *"rocksdb_matrix50_base_"* ]]; then
    echo "current_case: base_fill (one-time 50GB load)"
  else
    echo "current_case: not started yet"
  fi
  echo
  if [[ -n "$db_line" ]]; then
    echo "db_bench_pid: $db_pid"
    echo "benchmarks: ${bench:-NA}"
    if [[ -n "$seek_nexts" ]]; then
      echo "seek_nexts: $seek_nexts"
    fi
    if [[ "$target" -gt 0 ]]; then
      echo "progress_ops: ${progress:-NA} / $target $target_note (${pct}%)"
    else
      echo "progress_ops: ${progress:-NA} (target unknown)"
    fi
    echo "elapsed: $(human_duration "${elapsed:-0}")"
    if [[ "$eta" != "NA" && "$eta" != "-1" ]]; then
      echo "eta: $(human_duration "$eta")"
    else
      echo "eta: NA"
    fi
  else
    echo "db_bench_pid: none"
  fi
  echo
  echo "log: $RUNNER_LOG"
  echo "tip: tail -f $RUNNER_LOG"
}

while true; do
  render_once

  if [[ "$ONCE" == "1" ]]; then
    break
  fi

  total="$(total_runs)"
  done="$(done_runs)"
  skip="$(skipped_runs)"
  active="$(pgrep -fa 'run_standard_matrix_50gb.sh|run_release_50gb_quick.sh|db_bench' | grep -v 'watch_matrix_progress' || true)"
  if [[ -z "$active" && "$total" -gt 0 && $((done + skip)) -ge "$total" ]]; then
    echo
    echo "Matrix appears finished."
    break
  fi

  sleep "$REFRESH_SEC"
done
