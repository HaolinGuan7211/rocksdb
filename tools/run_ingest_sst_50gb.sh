#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LDB_BIN="${LDB_BIN:-$ROOT_DIR/build/tools/ldb}"
DB_BENCH="${DB_BENCH:-$ROOT_DIR/build/db_bench}"
GEN_SCRIPT="${GEN_SCRIPT:-$ROOT_DIR/tools/gen_kv_stream.py}"

if [[ ! -x "$LDB_BIN" ]]; then
  echo "missing ldb binary: $LDB_BIN" >&2
  echo "build with: cmake --build build --target ldb -j8" >&2
  exit 1
fi
if [[ ! -x "$DB_BENCH" ]]; then
  echo "missing db_bench binary: $DB_BENCH" >&2
  exit 1
fi
if [[ ! -f "$GEN_SCRIPT" ]]; then
  echo "missing generator: $GEN_SCRIPT" >&2
  exit 1
fi

EXP_DATE="${EXP_DATE:-$(date +%Y%m%d)}"
GLOBAL_EXP_ID="${GLOBAL_EXP_ID:-0}"
EXPERIMENT_NAME="${EXPERIMENT_NAME:-sst_ingest50gb}"
RUN_TIME="${RUN_TIME:-$(date +%H%M%S)}"
RUN_TAG="${RUN_TAG:-${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}_${RUN_TIME}}"

EXPERIMENT_ROOT="${EXPERIMENT_ROOT:-$ROOT_DIR/experiment}"
EXPERIMENT_DIR="${EXPERIMENT_DIR:-$EXPERIMENT_ROOT/${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}}"
RUN_DIR="${RUN_DIR:-$EXPERIMENT_DIR/run_results/$RUN_TAG}"
FIGURES_DIR="${FIGURES_DIR:-$EXPERIMENT_DIR/figures}"

TARGET_DB_GB="${TARGET_DB_GB:-50}"
KEY_SIZE="${KEY_SIZE:-16}"
VALUE_SIZE="${VALUE_SIZE:-256}"
LDB_COMPRESSION_TYPE="${LDB_COMPRESSION_TYPE:-no}"
DB_BENCH_COMPRESSION_TYPE="${DB_BENCH_COMPRESSION_TYPE:-none}"
CHUNK_KEYS="${CHUNK_KEYS:-2000000}"
INGEST_BEHIND="${INGEST_BEHIND:-0}"
MOVE_FILES="${MOVE_FILES:-1}"
THREADS="${THREADS:-16}"
USE_DIRECT="${USE_DIRECT:-true}"
KEY_FORMAT="${KEY_FORMAT:-db_bench_u64be_ascii0}" # legacy_k | db_bench_u64be_ascii0
SANITY_CHECK_FOUND="${SANITY_CHECK_FOUND:-1}"
SANITY_CHECK_READS="${SANITY_CHECK_READS:-1000}"
SANITY_MIN_FOUND_RATIO="${SANITY_MIN_FOUND_RATIO:-0.95}"

DB_ROOT="${DB_ROOT:-/tmp/rocksdb_sst_ingest_base_$RUN_TAG}"
DB_DIR="${DB_DIR:-$DB_ROOT/db}"
WAL_DIR="${WAL_DIR:-$DB_ROOT/wal}"
SST_DIR="${SST_DIR:-$DB_ROOT/external_sst}"

mkdir -p "$RUN_DIR" "$FIGURES_DIR" "$DB_DIR" "$WAL_DIR" "$SST_DIR" "$EXPERIMENT_DIR"

target_bytes="$(awk -v g="$TARGET_DB_GB" 'BEGIN { printf "%.0f", g * 1024 * 1024 * 1024 }')"
kv_bytes=$((KEY_SIZE + VALUE_SIZE))
NUM_KEYS=$((target_bytes / kv_bytes))
TOTAL_CHUNKS=$(((NUM_KEYS + CHUNK_KEYS - 1) / CHUNK_KEYS))

cat >"$RUN_DIR/config.txt" <<EOF
RUN_TAG=$RUN_TAG
EXP_DATE=$EXP_DATE
GLOBAL_EXP_ID=$GLOBAL_EXP_ID
EXPERIMENT_NAME=$EXPERIMENT_NAME
RUN_TIME=$RUN_TIME
EXPERIMENT_DIR=$EXPERIMENT_DIR
RUN_DIR=$RUN_DIR
LDB_BIN=$LDB_BIN
DB_BENCH=$DB_BENCH
GEN_SCRIPT=$GEN_SCRIPT
TARGET_DB_GB=$TARGET_DB_GB
KEY_SIZE=$KEY_SIZE
VALUE_SIZE=$VALUE_SIZE
KEY_FORMAT=$KEY_FORMAT
LDB_COMPRESSION_TYPE=$LDB_COMPRESSION_TYPE
DB_BENCH_COMPRESSION_TYPE=$DB_BENCH_COMPRESSION_TYPE
CHUNK_KEYS=$CHUNK_KEYS
NUM_KEYS=$NUM_KEYS
TOTAL_CHUNKS=$TOTAL_CHUNKS
INGEST_BEHIND=$INGEST_BEHIND
MOVE_FILES=$MOVE_FILES
THREADS=$THREADS
USE_DIRECT=$USE_DIRECT
SANITY_CHECK_FOUND=$SANITY_CHECK_FOUND
SANITY_CHECK_READS=$SANITY_CHECK_READS
SANITY_MIN_FOUND_RATIO=$SANITY_MIN_FOUND_RATIO
DB_DIR=$DB_DIR
WAL_DIR=$WAL_DIR
SST_DIR=$SST_DIR
EOF

cat >"$EXPERIMENT_DIR/experiment_plan.md" <<EOF
# Experiment Plan

## Identity
- exp_date: $EXP_DATE
- global_exp_id: $GLOBAL_EXP_ID
- experiment_name: $EXPERIMENT_NAME
- run_tag: $RUN_TAG

## Objective
- build ~${TARGET_DB_GB}GB dataset by generating sorted external SST files and ingesting into RocksDB directly.
- avoid heavy WAL/memtable/compaction path from point-by-point Put.

## Key Parameters
- data_scale: target_db_gb=$TARGET_DB_GB, num_keys=$NUM_KEYS
- key_shape: key_size=$KEY_SIZE, value_size=$VALUE_SIZE
- key_format: $KEY_FORMAT
- sst_build: chunk_keys=$CHUNK_KEYS, total_chunks=$TOTAL_CHUNKS
- ingest_options: move_files=$MOVE_FILES, ingest_behind=$INGEST_BEHIND
- rocksdb_options: ldb_compression_type=$LDB_COMPRESSION_TYPE
- db_paths: db=$DB_DIR, wal=$WAL_DIR, external_sst=$SST_DIR

## Expected Result
- complete base data load with lower write amplification than Put-based fillrandom.
- produce ingest log and final stats in run directory.
EOF

echo "[$(date '+%F %T')] start sst ingest load" | tee -a "$RUN_DIR/runner.log"
echo "target: ${TARGET_DB_GB}GB raw, num_keys=$NUM_KEYS, chunks=$TOTAL_CHUNKS" | tee -a "$RUN_DIR/runner.log"

move_flag=()
if [[ "$MOVE_FILES" == "1" ]]; then
  move_flag+=(--move_files)
fi
behind_flag=()
if [[ "$INGEST_BEHIND" == "1" ]]; then
  behind_flag+=(--ingest_behind)
fi

hex_flag=()
value_hex_flag=()
if [[ "$KEY_FORMAT" == "db_bench_u64be_ascii0" ]]; then
  hex_flag+=(--hex)
  value_hex_flag+=(--value-hex)
fi

start_key=0
for ((chunk_id = 1; chunk_id <= TOTAL_CHUNKS; chunk_id++)); do
  remain=$((NUM_KEYS - start_key))
  if ((remain <= 0)); then
    break
  fi
  chunk_count=$CHUNK_KEYS
  if ((remain < CHUNK_KEYS)); then
    chunk_count=$remain
  fi

  sst_path="$SST_DIR/chunk_${chunk_id}.sst"
  chunk_start_ts="$(date +%s)"
  echo "[$(date '+%F %T')] chunk ${chunk_id}/${TOTAL_CHUNKS} generate+write: start=$start_key count=$chunk_count" | tee -a "$RUN_DIR/runner.log"

  python3 "$GEN_SCRIPT" \
    --start="$start_key" \
    --count="$chunk_count" \
    --key-size="$KEY_SIZE" \
    --value-size="$VALUE_SIZE" \
    --key-format="$KEY_FORMAT" \
    "${value_hex_flag[@]}" \
    | "$LDB_BIN" \
      --db="$DB_DIR" \
      --create_if_missing \
      "${hex_flag[@]}" \
      --compression_type="$LDB_COMPRESSION_TYPE" \
      write_extern_sst "$sst_path" \
      >"$RUN_DIR/chunk_${chunk_id}_write.log" 2>&1

  echo "[$(date '+%F %T')] chunk ${chunk_id}/${TOTAL_CHUNKS} ingest $sst_path" | tee -a "$RUN_DIR/runner.log"
  "$LDB_BIN" \
    --db="$DB_DIR" \
    --create_if_missing \
    ingest_extern_sst "$sst_path" \
    "${move_flag[@]}" \
    "${behind_flag[@]}" \
    >"$RUN_DIR/chunk_${chunk_id}_ingest.log" 2>&1

  # If move_files is disabled or unsupported, remove source file after ingest.
  rm -f "$sst_path" || true

  start_key=$((start_key + chunk_count))
  chunk_end_ts="$(date +%s)"
  used_s=$((chunk_end_ts - chunk_start_ts))
  pct="$(awk -v x="$start_key" -v t="$NUM_KEYS" 'BEGIN { printf "%.2f", (x*100.0)/t }')"
  db_bytes="$(du -sb "$DB_DIR" | awk '{print $1}')"
  db_gib="$(awk -v x="$db_bytes" 'BEGIN { printf "%.3f", x/(1024*1024*1024) }')"
  echo "[$(date '+%F %T')] chunk ${chunk_id} done, elapsed=${used_s}s, keys=$start_key/$NUM_KEYS (${pct}%), db_size=${db_gib}GiB" | tee -a "$RUN_DIR/runner.log"
done

echo "[$(date '+%F %T')] final stats" | tee -a "$RUN_DIR/runner.log"
"$DB_BENCH" \
  --db="$DB_DIR" \
  --wal_dir="$WAL_DIR" \
  --use_existing_db=1 \
  --benchmarks=stats \
  --statistics \
  --threads="$THREADS" \
  --num="$NUM_KEYS" \
  --key_size="$KEY_SIZE" \
  --compression_type="$DB_BENCH_COMPRESSION_TYPE" \
  --use_direct_reads="$USE_DIRECT" \
  --use_direct_io_for_flush_and_compaction="$USE_DIRECT" \
  >"$RUN_DIR/99_stats.log" 2>&1

if [[ "$SANITY_CHECK_FOUND" == "1" ]]; then
  sanity_out="$("$DB_BENCH" \
    --db="$DB_DIR" \
    --wal_dir="$WAL_DIR" \
    --use_existing_db=1 \
    --benchmarks=seekrandom \
    --num="$NUM_KEYS" \
    --reads="$SANITY_CHECK_READS" \
    --threads=1 \
    --key_size="$KEY_SIZE" \
    --compression_type="$DB_BENCH_COMPRESSION_TYPE" \
    --cache_size="$((1<<20))" \
    --use_direct_reads="$USE_DIRECT" \
    --use_direct_io_for_flush_and_compaction="$USE_DIRECT" \
    --seek_nexts=1 2>&1)"
  echo "$sanity_out" >"$RUN_DIR/98_sanity_seekrandom.log"
  found="$(echo "$sanity_out" | sed -nE 's/.*\(([0-9]+) of ([0-9]+) found\).*/\1/p' | tail -n1)"
  total="$(echo "$sanity_out" | sed -nE 's/.*\(([0-9]+) of ([0-9]+) found\).*/\2/p' | tail -n1)"
  if [[ -z "$found" || -z "$total" || "$total" == "0" ]]; then
    echo "sanity_check_parse_error=1" >>"$RUN_DIR/config.txt"
    echo "SANITY_CHECK parse failed. See 98_sanity_seekrandom.log" | tee -a "$RUN_DIR/runner.log"
    exit 2
  fi
  found_ratio="$(awk -v f="$found" -v t="$total" 'BEGIN { printf "%.6f", f/t }')"
  echo "sanity_seekrandom_found_ratio=$found_ratio ($found/$total)" | tee -a "$RUN_DIR/runner.log"
  echo "sanity_seekrandom_found_ratio=$found_ratio" >>"$RUN_DIR/config.txt"
  if ! awk -v r="$found_ratio" -v m="$SANITY_MIN_FOUND_RATIO" 'BEGIN { exit !(r >= m) }'; then
    echo "SANITY_CHECK failed: found ratio $found_ratio < $SANITY_MIN_FOUND_RATIO" | tee -a "$RUN_DIR/runner.log"
    exit 3
  fi
fi

db_bytes="$(du -sb "$DB_DIR" | awk '{print $1}')"
raw_bytes=$((NUM_KEYS * kv_bytes))
space_amp="$(awk -v d="$db_bytes" -v r="$raw_bytes" 'BEGIN { if (r>0) printf "%.4f", d/r; else print "NA" }')"

cat >"$EXPERIMENT_DIR/experiment_result.md" <<EOF
# Experiment Result

## Identity
- run_tag: $RUN_TAG
- exp_date: $EXP_DATE
- global_exp_id: $GLOBAL_EXP_ID
- experiment_name: $EXPERIMENT_NAME

## Load Summary
- target_db_gb: $TARGET_DB_GB
- num_keys: $NUM_KEYS
- key_size/value_size: $KEY_SIZE/$VALUE_SIZE
- total_chunks: $TOTAL_CHUNKS
- ingest_options: move_files=$MOVE_FILES, ingest_behind=$INGEST_BEHIND
- key_format: $KEY_FORMAT
- sanity_check: enabled=$SANITY_CHECK_FOUND, reads=$SANITY_CHECK_READS, min_found_ratio=$SANITY_MIN_FOUND_RATIO

## Size Snapshot
- logical_raw_bytes: $raw_bytes
- db_bytes: $db_bytes
- space_ratio_db_over_raw: $space_amp

## Artifacts
- run_dir: $RUN_DIR
- runner_log: $RUN_DIR/runner.log
- stats_log: $RUN_DIR/99_stats.log
EOF

echo "[$(date '+%F %T')] done, run_dir=$RUN_DIR" | tee -a "$RUN_DIR/runner.log"
echo "Done: $EXPERIMENT_DIR"
