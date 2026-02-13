#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="$ROOT_DIR/tools/run_shortscan_compare.sh"
CACHE_FILE="$ROOT_DIR/build/CMakeCache.txt"

if [[ ! -x "$RUN_SCRIPT" ]]; then
  echo "missing runner: $RUN_SCRIPT" >&2
  exit 1
fi

if [[ ! -f "$CACHE_FILE" ]] || ! rg -q "^CMAKE_BUILD_TYPE:STRING=Release$" "$CACHE_FILE"; then
  echo "Release build is required. Please run: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j" >&2
  exit 1
fi

TARGET_DB_GB="${TARGET_DB_GB:-100}"
KEY_SIZE="${KEY_SIZE:-16}"
VALUE_SIZE="${VALUE_SIZE:-256}"
THREADS="${THREADS:-16}"
FILL_THREADS="${FILL_THREADS:-1}"
CACHE_SIZES="${CACHE_SIZES:-$((4<<30)),$((8<<30)),$((16<<30))}"
EXP_DATE="${EXP_DATE:-$(date +%Y%m%d)}"
GLOBAL_EXP_ID="${GLOBAL_EXP_ID:-0}"
EXPERIMENT_NAME="${EXPERIMENT_NAME:-release100gb_readmodes}"
RUN_TIME="${RUN_TIME:-$(date +%H%M%S)}"
RUN_TAG="${RUN_TAG:-${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}_${RUN_TIME}}"
EXPERIMENT_ROOT="${EXPERIMENT_ROOT:-$ROOT_DIR/experiment}"
EXPERIMENT_DIR="${EXPERIMENT_DIR:-$EXPERIMENT_ROOT/${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}}"
RUN_OUT_DIR="${RUN_OUT_DIR:-$EXPERIMENT_DIR/run_results/$RUN_TAG}"
FIGURES_DIR="${FIGURES_DIR:-$EXPERIMENT_DIR/figures}"

DB_ROOT="${DB_ROOT:-/tmp/rocksdb_release_100gb_$RUN_TAG}"
WAL_ROOT="${WAL_ROOT:-/tmp/rocksdb_release_100gb_wal_$RUN_TAG}"
mkdir -p "$DB_ROOT" "$WAL_ROOT" "$EXPERIMENT_DIR" "$FIGURES_DIR"

# Approximate raw KV bytes. For the 100GB requirement, use raw payload as target baseline.
target_bytes="$(awk -v g="$TARGET_DB_GB" 'BEGIN { printf "%.0f", g * 1024 * 1024 * 1024 }')"
per_kv_bytes=$((KEY_SIZE + VALUE_SIZE))
NUM_KEYS=$((target_bytes / per_kv_bytes))

# Keep workload proportions consistent with previous plan.
REALISTIC_READS=$((NUM_KEYS * 4))
STEP200_READS=$((NUM_KEYS / 10))
WORST_READS_1=$((NUM_KEYS * 13 / 10))
WORST_READS_4=$((NUM_KEYS * 4 / 10))
WORST_READS_20=$((NUM_KEYS * 2 / 10))
WORST_READS_200=$((NUM_KEYS * 8 / 100))
WORST_READS_10000=$((NUM_KEYS * 2 / 100))

# Preflight disk check (20% headroom).
check_path="$DB_ROOT"
if [[ ! -d "$check_path" ]]; then
  check_path="$(dirname "$check_path")"
fi
avail_bytes="$(df -B1 --output=avail "$check_path" | tail -n 1 | tr -d ' ')"
required_bytes="$(awk -v t="$target_bytes" 'BEGIN { printf "%.0f", t * 1.2 }')"
if (( avail_bytes < required_bytes )); then
  echo "Not enough disk for 100GB load experiment." >&2
  echo "required >= $required_bytes bytes (target * 1.2), available = $avail_bytes bytes on $check_path" >&2
  exit 1
fi

# If caller wants strict OS cache drop, they must provide a privileged command.
if [[ "${CLEAR_OS_CACHE_BETWEEN_STEPS:-1}" == "1" ]] && [[ -z "${DROP_CACHE_CMD:-}" ]]; then
  echo "DROP_CACHE_CMD is empty. Example:" >&2
  echo "  DROP_CACHE_CMD='sync; echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null'" >&2
  exit 1
fi

echo "Running release read-mode experiment:"
echo "  RUN_TAG=$RUN_TAG"
echo "  NUM_KEYS=$NUM_KEYS (~${TARGET_DB_GB}GB raw target)"
echo "  FILL_THREADS=$FILL_THREADS, WORKLOAD_THREADS=$THREADS"
echo "  CACHE_SIZES=$CACHE_SIZES"
echo "  DB_ROOT=$DB_ROOT"
echo "  EXPERIMENT_DIR=$EXPERIMENT_DIR"

RUN_TAG="$RUN_TAG" \
EXP_DATE="$EXP_DATE" \
GLOBAL_EXP_ID="$GLOBAL_EXP_ID" \
EXPERIMENT_NAME="$EXPERIMENT_NAME" \
RUN_TIME="$RUN_TIME" \
EXPERIMENT_ROOT="$EXPERIMENT_ROOT" \
EXPERIMENT_DIR="$EXPERIMENT_DIR" \
OUT_DIR="$RUN_OUT_DIR" \
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
CACHE_SIZES="$CACHE_SIZES" \
THREADS="$THREADS" \
FILL_THREADS="$FILL_THREADS" \
KEY_SIZE="$KEY_SIZE" \
VALUE_SIZE="$VALUE_SIZE" \
COMPRESSION_TYPE=none \
USE_DIRECT=true \
ISOLATE_BY_CACHE=0 \
MIX_GET_RATIO=0.10 \
MIX_PUT_RATIO=0.0 \
MIX_SEEK_RATIO=0.90 \
EXPERIMENT_TITLE="${EXPERIMENT_TITLE:-Release 100GB read-mode benchmark}" \
EXPERIMENT_OBJECTIVE="${EXPERIMENT_OBJECTIVE:-Measure current format behavior under larger data scale and multiple read locality modes.}" \
EXPERIMENT_VARIABLES="${EXPERIMENT_VARIABLES:-cache_size,read_mode,seek_nexts}" \
EXPERIMENT_EXPECTATION="${EXPERIMENT_EXPECTATION:-With larger cache at fixed data size, realistic read throughput should improve and tail latency should not regress.}" \
EXPECTED_CACHE_TPUT_TREND="${EXPECTED_CACHE_TPUT_TREND:-increase}" \
EXPECTED_CACHE_P99_TREND="${EXPECTED_CACHE_P99_TREND:-decrease}" \
KEY_LOCALITY_DESC="${KEY_LOCALITY_DESC:-mixgraph locality + seekrandom (seek_nexts=200 and worst-locality set).}" \
ROCKSDB_BUILTIN_OPTIMIZATIONS="${ROCKSDB_BUILTIN_OPTIMIZATIONS:-use_direct_reads=true,use_direct_io_for_flush_and_compaction=true,compression_type=none}" \
CLEAR_OS_CACHE_BETWEEN_STEPS="${CLEAR_OS_CACHE_BETWEEN_STEPS:-1}" \
DROP_CACHE_CMD="${DROP_CACHE_CMD:-}" \
DB_DIR="$DB_ROOT/db" \
WAL_DIR="$WAL_ROOT/wal" \
"$RUN_SCRIPT"

echo "Done: $EXPERIMENT_DIR"
