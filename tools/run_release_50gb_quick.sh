#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="$ROOT_DIR/tools/run_shortscan_compare.sh"
CACHE_FILE="$ROOT_DIR/build/CMakeCache.txt"
DB_BENCH="${DB_BENCH:-$ROOT_DIR/build/db_bench}"

if [[ ! -x "$RUN_SCRIPT" ]]; then
  echo "missing runner: $RUN_SCRIPT" >&2
  exit 1
fi
if [[ ! -x "$DB_BENCH" ]]; then
  echo "missing db_bench: $DB_BENCH" >&2
  exit 1
fi

if [[ ! -f "$CACHE_FILE" ]] || ! rg -q "^CMAKE_BUILD_TYPE:STRING=Release$" "$CACHE_FILE"; then
  echo "Release build is required." >&2
  echo "Run: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8" >&2
  exit 1
fi

TARGET_DB_GB="${TARGET_DB_GB:-50}"
KEY_SIZE="${KEY_SIZE:-16}"
VALUE_SIZE="${VALUE_SIZE:-256}"
THREADS="${THREADS:-16}"
FILL_THREADS="${FILL_THREADS:-8}"
COMPRESSION_TYPE="${COMPRESSION_TYPE:-none}"
USE_DIRECT="${USE_DIRECT:-true}"
MIX_GET_RATIO="${MIX_GET_RATIO:-0.15}"
MIX_PUT_RATIO="${MIX_PUT_RATIO:-0.05}"
MIX_SEEK_RATIO="${MIX_SEEK_RATIO:-0.80}"
CLEAR_OS_CACHE_BETWEEN_STEPS="${CLEAR_OS_CACHE_BETWEEN_STEPS:-0}"
DROP_CACHE_CMD="${DROP_CACHE_CMD:-}"
SANITY_CHECK_FOUND="${SANITY_CHECK_FOUND:-1}"
SANITY_CHECK_READS="${SANITY_CHECK_READS:-1000}"
SANITY_MIN_FOUND_RATIO="${SANITY_MIN_FOUND_RATIO:-0.01}"

# One cache size per run by default, so one experiment can finish ~30 min.
CACHE_SIZES="${CACHE_SIZES:-$((8<<30))}"

# Budget-oriented TOTAL reads (across all threads).
MIXGRAPH_TOTAL_READS="${MIXGRAPH_TOTAL_READS:-12000000}"
SEEK200_TOTAL_READS="${SEEK200_TOTAL_READS:-1000000}"
WORST1_TOTAL_READS="${WORST1_TOTAL_READS:-4000000}"
WORST4_TOTAL_READS="${WORST4_TOTAL_READS:-2000000}"
WORST20_TOTAL_READS="${WORST20_TOTAL_READS:-1000000}"
WORST200_TOTAL_READS="${WORST200_TOTAL_READS:-400000}"
WORST10000_TOTAL_READS="${WORST10000_TOTAL_READS:-30000}"  # step08 uses 8 threads in runner

SKIP_FILL="${SKIP_FILL:-0}"
EXP_DATE="${EXP_DATE:-$(date +%Y%m%d)}"
GLOBAL_EXP_ID="${GLOBAL_EXP_ID:-0}"
EXPERIMENT_NAME="${EXPERIMENT_NAME:-release50gb_quick}"
RUN_TIME="${RUN_TIME:-$(date +%H%M%S)}"
RUN_TAG="${RUN_TAG:-${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}_${RUN_TIME}}"
EXPERIMENT_ROOT="${EXPERIMENT_ROOT:-$ROOT_DIR/experiment}"
EXPERIMENT_DIR="${EXPERIMENT_DIR:-$EXPERIMENT_ROOT/${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}}"
RUN_OUT_DIR="${RUN_OUT_DIR:-$EXPERIMENT_DIR/run_results/$RUN_TAG}"
FIGURES_DIR="${FIGURES_DIR:-$EXPERIMENT_DIR/figures}"
DB_ROOT="${DB_ROOT:-/tmp/rocksdb_release_50gb_$RUN_TAG}"
WAL_ROOT="${WAL_ROOT:-/tmp/rocksdb_release_50gb_wal_$RUN_TAG}"

mkdir -p "$DB_ROOT" "$WAL_ROOT" "$EXPERIMENT_DIR" "$FIGURES_DIR"

target_bytes="$(awk -v g="$TARGET_DB_GB" 'BEGIN { printf "%.0f", g * 1024 * 1024 * 1024 }')"
per_kv_bytes=$((KEY_SIZE + VALUE_SIZE))
NUM_KEYS=$((target_bytes / per_kv_bytes))

ceil_div() {
  local a="$1"
  local b="$2"
  echo $(((a + b - 1) / b))
}

# run_shortscan_compare expects --reads as per-thread count.
REALISTIC_READS="$(ceil_div "$MIXGRAPH_TOTAL_READS" "$THREADS")"
STEP200_READS="$(ceil_div "$SEEK200_TOTAL_READS" "$THREADS")"
WORST_READS_1="$(ceil_div "$WORST1_TOTAL_READS" "$THREADS")"
WORST_READS_4="$(ceil_div "$WORST4_TOTAL_READS" "$THREADS")"
WORST_READS_20="$(ceil_div "$WORST20_TOTAL_READS" "$THREADS")"
WORST_READS_200="$(ceil_div "$WORST200_TOTAL_READS" "$THREADS")"
WORST_READS_10000="$(ceil_div "$WORST10000_TOTAL_READS" 8)"

if [[ "$SKIP_FILL" != "1" ]]; then
  check_path="$DB_ROOT"
  avail_bytes="$(df -B1 --output=avail "$check_path" | tail -n 1 | tr -d ' ')"
  required_bytes="$(awk -v t="$target_bytes" 'BEGIN { printf "%.0f", t * 1.15 }')"
  if (( avail_bytes < required_bytes )); then
    echo "Not enough disk for target ${TARGET_DB_GB}GB dataset." >&2
    echo "required >= $required_bytes bytes, available = $avail_bytes bytes on $check_path" >&2
    exit 1
  fi
else
  if [[ ! -f "$DB_ROOT/db/CURRENT" ]]; then
    echo "SKIP_FILL=1 but existing DB not found: $DB_ROOT/db" >&2
    exit 1
  fi
fi

run_keyspace_sanity_check() {
  local out
  out="$("$DB_BENCH" \
    --db="$DB_ROOT/db" \
    --wal_dir="$WAL_ROOT/wal" \
    --use_existing_db=1 \
    --benchmarks=seekrandom \
    --num="$NUM_KEYS" \
    --reads="$SANITY_CHECK_READS" \
    --threads=1 \
    --key_size="$KEY_SIZE" \
    --compression_type="$COMPRESSION_TYPE" \
    --cache_size="$((1<<20))" \
    --use_direct_reads="$USE_DIRECT" \
    --use_direct_io_for_flush_and_compaction="$USE_DIRECT" \
    --seek_nexts=1 2>&1)"

  local found total ratio
  found="$(echo "$out" | sed -nE 's/.*\(([0-9]+) of ([0-9]+) found\).*/\1/p' | tail -n1)"
  total="$(echo "$out" | sed -nE 's/.*\(([0-9]+) of ([0-9]+) found\).*/\2/p' | tail -n1)"
  if [[ -z "$found" || -z "$total" || "$total" == "0" ]]; then
    echo "SANITY_CHECK: unable to parse seekrandom found-ratio." >&2
    echo "$out" >&2
    exit 1
  fi
  ratio="$(awk -v f="$found" -v t="$total" 'BEGIN { printf "%.6f", f/t }')"
  echo "SANITY_CHECK seekrandom_found_ratio=$ratio ($found/$total)"
  if ! awk -v r="$ratio" -v m="$SANITY_MIN_FOUND_RATIO" 'BEGIN { exit !(r >= m) }'; then
    echo "SANITY_CHECK failed: found ratio ${ratio} < ${SANITY_MIN_FOUND_RATIO}." >&2
    echo "Likely keyspace mismatch between workload key generator and loaded DB keys." >&2
    exit 2
  fi
}

if [[ "$SANITY_CHECK_FOUND" == "1" ]]; then
  run_keyspace_sanity_check
fi

echo "Quick 50GB experiment config:"
echo "  RUN_TAG=$RUN_TAG"
echo "  EXP_DATE=$EXP_DATE GLOBAL_EXP_ID=$GLOBAL_EXP_ID EXPERIMENT_NAME=$EXPERIMENT_NAME"
echo "  EXPERIMENT_DIR=$EXPERIMENT_DIR"
echo "  NUM_KEYS=$NUM_KEYS (~${TARGET_DB_GB}GB raw target)"
echo "  CACHE_SIZES=$CACHE_SIZES"
echo "  THREADS=$THREADS FILL_THREADS=$FILL_THREADS"
echo "  MIX_RATIO(get/put/seek)=$MIX_GET_RATIO/$MIX_PUT_RATIO/$MIX_SEEK_RATIO"
echo "  TOTAL_READS: mixgraph=$MIXGRAPH_TOTAL_READS seek200=$SEEK200_TOTAL_READS worst1=$WORST1_TOTAL_READS worst4=$WORST4_TOTAL_READS worst20=$WORST20_TOTAL_READS worst200=$WORST200_TOTAL_READS worst10000=$WORST10000_TOTAL_READS"
echo "  PER_THREAD_READS: mixgraph=$REALISTIC_READS seek200=$STEP200_READS worst1=$WORST_READS_1 worst4=$WORST_READS_4 worst20=$WORST_READS_20 worst200=$WORST_READS_200 worst10000=$WORST_READS_10000"

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
COMPRESSION_TYPE="$COMPRESSION_TYPE" \
USE_DIRECT="$USE_DIRECT" \
SKIP_FILL="$SKIP_FILL" \
ISOLATE_BY_CACHE=0 \
MIX_GET_RATIO="$MIX_GET_RATIO" \
MIX_PUT_RATIO="$MIX_PUT_RATIO" \
MIX_SEEK_RATIO="$MIX_SEEK_RATIO" \
EXPERIMENT_TITLE="${EXPERIMENT_TITLE:-Release 50GB quick read-mode sanity}" \
EXPERIMENT_OBJECTIVE="${EXPERIMENT_OBJECTIVE:-Validate 50GB load path, read workload execution, and metric collection quality under release build.}" \
EXPERIMENT_VARIABLES="${EXPERIMENT_VARIABLES:-cache_size,read_mode,seek_nexts}" \
EXPERIMENT_EXPECTATION="${EXPERIMENT_EXPECTATION:-At 50GB scale, larger cache should generally improve throughput and reduce seek tail latency for realistic locality.}" \
EXPECTED_CACHE_TPUT_TREND="${EXPECTED_CACHE_TPUT_TREND:-increase}" \
EXPECTED_CACHE_P99_TREND="${EXPECTED_CACHE_P99_TREND:-decrease}" \
KEY_LOCALITY_DESC="${KEY_LOCALITY_DESC:-mixgraph locality with key_dist/keyrange/iter distributions; plus seekrandom worst-locality injection.}" \
ROCKSDB_BUILTIN_OPTIMIZATIONS="${ROCKSDB_BUILTIN_OPTIMIZATIONS:-use_direct_reads=true,use_direct_io_for_flush_and_compaction=true,compression_type=none}" \
CLEAR_OS_CACHE_BETWEEN_STEPS="$CLEAR_OS_CACHE_BETWEEN_STEPS" \
DROP_CACHE_CMD="$DROP_CACHE_CMD" \
DB_DIR="$DB_ROOT/db" \
WAL_DIR="$WAL_ROOT/wal" \
"$RUN_SCRIPT"
echo "Done: $EXPERIMENT_DIR"
