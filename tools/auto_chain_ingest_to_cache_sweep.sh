#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INGEST_EXP_DIR="${INGEST_EXP_DIR:-$ROOT_DIR/experiment/20260205_exp2_base_ingest50gb_dbbenchkey}"
BASE_DB_ROOT="${BASE_DB_ROOT:-/tmp/rocksdb_sst_ingest_base_20260205_experiment0_dbbenchfmt}"

EXP_DATE="${EXP_DATE:-$(date +%Y%m%d)}"
EXPERIMENT_ID="${EXPERIMENT_ID:-3}"
MATRIX_NAME="${MATRIX_NAME:-cache_sweep_50gb}"
MATRIX_TAG="${MATRIX_TAG:-${EXP_DATE}_experiment${EXPERIMENT_ID}_${MATRIX_NAME}}"
MATRIX_DIR="${MATRIX_DIR:-$ROOT_DIR/experiment/$MATRIX_TAG}"

CACHE_SWEEP_GIBS="${CACHE_SWEEP_GIBS:-1,2,4}"
MAX_RUNS="${MAX_RUNS:-3}"
MIN_FREE_GB="${MIN_FREE_GB:-8}"

CHAIN_LOG="$ROOT_DIR/experiment/${EXP_DATE}_experiment${EXPERIMENT_ID}_chain.log"

find_latest_ingest_runner_log() {
  ls -1dt "$INGEST_EXP_DIR"/run_results/*/runner.log 2>/dev/null | head -n1
}

echo "[$(date '+%F %T')] chain start: ingest=$INGEST_EXP_DIR matrix=$MATRIX_DIR" | tee -a "$CHAIN_LOG"

runner_log="$(find_latest_ingest_runner_log)"
if [[ -z "$runner_log" ]]; then
  echo "[$(date '+%F %T')] waiting for ingest runner.log..." | tee -a "$CHAIN_LOG"
fi
while [[ -z "$runner_log" ]]; do
  sleep 15
  runner_log="$(find_latest_ingest_runner_log)"
done

echo "[$(date '+%F %T')] watch ingest log: $runner_log" | tee -a "$CHAIN_LOG"
while true; do
  if rg -q "done, run_dir=" "$runner_log"; then
    echo "[$(date '+%F %T')] ingest done marker found." | tee -a "$CHAIN_LOG"
    break
  fi
  if ! pgrep -fa "run_ingest_sst_50gb.sh" >/dev/null; then
    echo "[$(date '+%F %T')] ingest process ended without done marker. abort." | tee -a "$CHAIN_LOG"
    exit 2
  fi
  sleep 30
done

if [[ ! -f "$BASE_DB_ROOT/db/CURRENT" ]]; then
  echo "[$(date '+%F %T')] base db missing after ingest: $BASE_DB_ROOT/db/CURRENT" | tee -a "$CHAIN_LOG"
  exit 3
fi

echo "[$(date '+%F %T')] launch cache sweep matrix..." | tee -a "$CHAIN_LOG"
mkdir -p "$MATRIX_DIR"

(
  while [[ ! -f "$MATRIX_DIR/matrix_runner.log" ]]; do
    sleep 2
  done
  python3 "$ROOT_DIR/tools/monitor_matrix_run.py" \
    --matrix-dir "$MATRIX_DIR" \
    --refresh 20 \
    --stall-seconds 900 \
    --min-free-gb "$MIN_FREE_GB" \
    --stop-on-fatal 1 \
    >"$MATRIX_DIR/monitor_stdout.log" 2>&1
) &

EXP_DATE="$EXP_DATE" \
EXPERIMENT_ID="$EXPERIMENT_ID" \
MATRIX_NAME="$MATRIX_NAME" \
MATRIX_TAG="$MATRIX_TAG" \
MATRIX_DIR="$MATRIX_DIR" \
CACHE_SWEEP_GIBS="$CACHE_SWEEP_GIBS" \
MAX_RUNS="$MAX_RUNS" \
PREPARE_BASE_DB=0 \
BASE_DB_ROOT="$BASE_DB_ROOT" \
USE_DIRECT=true \
SKIP_IF_EXISTS=0 \
SANITY_CHECK_FOUND=1 \
tools/run_standard_matrix_50gb.sh 2>&1 | tee "$MATRIX_DIR/matrix_runner.log"

python3 "$ROOT_DIR/tools/plot_matrix_results.py" --matrix-dir "$MATRIX_DIR" 2>&1 | tee -a "$MATRIX_DIR/matrix_runner.log"
echo "[$(date '+%F %T')] chain done: $MATRIX_DIR" | tee -a "$CHAIN_LOG"
