#!/usr/bin/env bash
set -euo pipefail

# Exp46: Gate3 "probe calibration" + "paper-style latency sweep"
#
# Task 1 (calibration):
#   Sweep WRITE_BUFFER_SIZE/TARGET_FILE_SIZE_BASE to reduce filter_probe_total
#   into a more realistic range (target: 10..200).
#
# Task 2 (verification):
#   On the calibrated point, run a 4-point simulated media latency sweep:
#     75ns, 750ns, 7.5us, 75us
#   and check the wait→CPU trend remains (paper-style).
#
# Notes:
# - Uses readmissing + cache=500MB and caches index+filter blocks.
# - Uses bloom_bits=2 to introduce controlled false-positives so the media sweep
#   can actually move the wait_share (otherwise a pure miss can become "all CPU").
# - Uses sample probe only (typical-op trend) to avoid tail selection bias.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB_BENCH="${DB_BENCH:-$ROOT_DIR/build/db_bench}"

EXP_DATE="${EXP_DATE:-$(date +%Y%m%d)}"
GLOBAL_EXP_ID="${GLOBAL_EXP_ID:-46}"
EXPERIMENT_NAME="${EXPERIMENT_NAME:-gate3_probe_calibrate_and_sweep}"
RUN_TIME="${RUN_TIME:-$(date +%H%M%S)}"
EXPERIMENT_DIR="${EXPERIMENT_DIR:-$ROOT_DIR/experiment/${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}}"

# If set, reuse an existing calibration_summary.csv under EXPERIMENT_DIR and only
# run Task 2 (verification sweep).
SKIP_CALIBRATION="${SKIP_CALIBRATION:-0}"

TARGET_DB_GIB="${TARGET_DB_GIB:-4.0}"
KEY_SIZE="${KEY_SIZE:-16}"
VALUE_SIZE="${VALUE_SIZE:-1024}"
COMPRESSION_TYPE="${COMPRESSION_TYPE:-lz4}"
COMPRESSION_RATIO="${COMPRESSION_RATIO:-0.10}"

# Calibration sweep points (bytes; comma-separated).
WBUF_LIST_BYTES="${WBUF_LIST_BYTES:-8388608,33554432,134217728}"       # 8MB,32MB,128MB
TFILE_LIST_BYTES="${TFILE_LIST_BYTES:-8388608,33554432,134217728}"    # match

# Calibration runtime: keep it small but enough samples for a stable median.
CALIB_THREADS="${CALIB_THREADS:-1}"
CALIB_READS="${CALIB_READS:-2000}"
CALIB_SAMPLE_INTERVAL_OPS="${CALIB_SAMPLE_INTERVAL_OPS:-1}"
CALIB_SAMPLE_MAX_SAMPLES="${CALIB_SAMPLE_MAX_SAMPLES:-5000}"

# Verification sweep (paper-style). Keep per-point small; we only need p50 trend.
XP_LATENCY_LIST_NS="${XP_LATENCY_LIST_NS:-75,750,7500,75000}"
SWEEP_THREADS="${SWEEP_THREADS:-1}"
SWEEP_READS="${SWEEP_READS:-5000}"
SWEEP_SAMPLE_INTERVAL_OPS="${SWEEP_SAMPLE_INTERVAL_OPS:-10}"
SWEEP_SAMPLE_MAX_SAMPLES="${SWEEP_SAMPLE_MAX_SAMPLES:-20000}"

# Gate3 knobs.
CACHE_SIZE="${CACHE_SIZE:-536870912}" # 500MB
BLOOM_BITS="${BLOOM_BITS:-2}"
PERF_LEVEL="${PERF_LEVEL:-4}"
FILL_BENCH="${FILL_BENCH:-fillrandom}"
FILL_THREADS="${FILL_THREADS:-4}"

# Overlap control.
FORCE_L0_OVERLAP="${FORCE_L0_OVERLAP:-1}" # keep 1 for Task 1; Task 1b can override
ALLOW_LIMITED_COMPACTION_IF_NEEDED="${ALLOW_LIMITED_COMPACTION_IF_NEEDED:-1}"

# SimFS.
USE_TMPFS_REDIRECT="${USE_TMPFS_REDIRECT:-1}"
TMPFS_ROOT="${TMPFS_ROOT:-/dev/shm/nvm_tmpfs_root.exp46_gate3_cal}"
SIMULATE_XP_BUSY_WAIT="${SIMULATE_XP_BUSY_WAIT:-0}"

DB_DIR="${DB_DIR:-/tmp/rocksdb_exp46_gate3_cal/db}"
WAL_DIR="${WAL_DIR:-/tmp/rocksdb_exp46_gate3_cal/wal}"

safe_rmtree() {
  local p="$1"
  if [[ -z "$p" ]]; then
    return 0
  fi
  python3 - "$p" <<'PY'
import shutil
import sys
from pathlib import Path
p = Path(sys.argv[1])
try:
    shutil.rmtree(p, ignore_errors=True)
except Exception:
    pass
PY
}

mkdir -p "$EXPERIMENT_DIR"

NUM_KEYS="$(awk -v gib="$TARGET_DB_GIB" -v k="$KEY_SIZE" -v v="$VALUE_SIZE" \
  'BEGIN { bytes=gib*1024*1024*1024; per=k+v; n=int(bytes/per); if(n<1)n=1; print n }')"

echo "[exp46] EXPERIMENT_DIR=$EXPERIMENT_DIR"
echo "[exp46] target_db_gib=$TARGET_DB_GIB key_size=$KEY_SIZE value_size=$VALUE_SIZE -> num_keys=$NUM_KEYS"
echo "[exp46] calib WBUF_LIST_BYTES=$WBUF_LIST_BYTES TFILE_LIST_BYTES=$TFILE_LIST_BYTES"
echo "[exp46] sweep XP_LATENCY_LIST_NS=$XP_LATENCY_LIST_NS"

common_simfs_args_base=(
  --histogram=1
  --disable_wal=1
  --perf_level="$PERF_LEVEL"
  --simulate_xp_nvm=1
  --simulate_xp_levels=0,1,2,3,4,5,6
  --simulate_xp_line_bytes=256
  --simulate_xp_buffer_bytes=16384
  --simulate_xp_rpq_depth=64
  --simulate_xp_wpq_depth=64
  --simulate_xp_wpq_submit_ns=100
  --simulate_xp_prefetch_hit_ns=120
  --simulate_xp_enable_prefetch=true
  --simulate_xp_busy_wait="$SIMULATE_XP_BUSY_WAIT"
)
if [[ "$USE_TMPFS_REDIRECT" == "1" ]]; then
  common_simfs_args_base+=(
    --simulate_xp_redirect_to_tmpfs=1
    --simulate_xp_tmpfs_root="$TMPFS_ROOT"
  )
fi

cache_args=(
  --cache_size="$CACHE_SIZE"
  --cache_index_and_filter_blocks=1
  --pin_top_level_index_and_filter=1
)

mkdir -p "$EXPERIMENT_DIR/run_results"
calib_csv="$EXPERIMENT_DIR/calibration_summary.csv"
if [[ "$SKIP_CALIBRATION" != "1" ]]; then
  echo "write_buffer_size,target_file_size_base,disable_auto_compactions,probe_median_overall,samples,run_dir" >"$calib_csv"
fi

IFS=',' read -r -a WBUF_ARRAY <<<"$WBUF_LIST_BYTES"
IFS=',' read -r -a TFILE_ARRAY <<<"$TFILE_LIST_BYTES"
if [[ "${#WBUF_ARRAY[@]}" -ne "${#TFILE_ARRAY[@]}" ]]; then
  echo "[exp46][fatal] WBUF_LIST_BYTES and TFILE_LIST_BYTES must have same length" >&2
  exit 1
fi

run_calibration_point() {
  local wbuf="$1"
  local tfile="$2"
  local disable_auto="$3"
  local point_tag="$4"

  local run_tag="${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}_${point_tag}_${RUN_TIME}"
  local out_dir="$EXPERIMENT_DIR/run_results/$run_tag"
  mkdir -p "$out_dir"

  safe_rmtree "$DB_DIR"
  safe_rmtree "$WAL_DIR"
  mkdir -p "$WAL_DIR"
  if [[ "$USE_TMPFS_REDIRECT" == "1" ]]; then
    safe_rmtree "$TMPFS_ROOT"
  fi

  local fill_extra_args=()
  if [[ "$disable_auto" == "1" ]]; then
    fill_extra_args+=(--disable_auto_compactions=1)
  fi

  local db_shape_args=(
    --key_size="$KEY_SIZE"
    --value_size="$VALUE_SIZE"
    --compression_type="$COMPRESSION_TYPE"
    --compression_ratio="$COMPRESSION_RATIO"
    --write_buffer_size="$wbuf"
    --max_write_buffer_number=4
    --min_write_buffer_number_to_merge=1
    --target_file_size_base="$tfile"
    --max_bytes_for_level_base=268435456
    --level0_file_num_compaction_trigger=8
    --max_background_compactions=4
    --bloom_bits=10
  )

  echo "[exp46][calib] fill wbuf=$wbuf tfile=$tfile disable_auto=$disable_auto"
  fill_cmd=(
    "$DB_BENCH"
    --db="$DB_DIR"
    --wal_dir="$WAL_DIR"
    --benchmarks="$FILL_BENCH",stats
    --statistics
    --num="$NUM_KEYS"
    --threads="$FILL_THREADS"
    "${common_simfs_args_base[@]}"
    --simulate_xp_latency_ns=750
    "${db_shape_args[@]}"
    "${fill_extra_args[@]}"
  )
  printf "%q " "${fill_cmd[@]}" >"$out_dir/00_fill.cmd"
  printf "\n" >>"$out_dir/00_fill.cmd"
  "${fill_cmd[@]}" 2>&1 | tee "$out_dir/00_fill.log"

  # Optional stabilize step when compactions are enabled.
  if [[ "$disable_auto" != "1" ]]; then
    echo "[exp46][calib] waitforcompaction (stabilize when auto compactions enabled)"
    wait_cmd=(
      "$DB_BENCH"
      --db="$DB_DIR"
      --wal_dir="$WAL_DIR"
      --use_existing_db=1
      --benchmarks=waitforcompaction,stats
      --statistics
      "${common_simfs_args_base[@]}"
      --simulate_xp_latency_ns=750
      "${db_shape_args[@]}"
    )
    printf "%q " "${wait_cmd[@]}" >"$out_dir/01_waitforcompaction.cmd"
    printf "\n" >>"$out_dir/01_waitforcompaction.cmd"
    "${wait_cmd[@]}" 2>&1 | tee "$out_dir/01_waitforcompaction.log"
  fi

  # Calibrate probe count using a small sample-probed readmissing run.
  local sample_csv="$out_dir/02_readmissing_cache_${CACHE_SIZE}.sample_probe.csv"
  local analysis_dir="$out_dir/analysis_readmissing_sample_cache_${CACHE_SIZE}"
  local read_cmd=(
    "$DB_BENCH"
    --db="$DB_DIR"
    --wal_dir="$WAL_DIR"
    --use_existing_db=1
    --benchmarks=readmissing,stats
    --statistics
    --num="$NUM_KEYS"
    --threads="$CALIB_THREADS"
    --reads="$CALIB_READS"
    --bloom_bits="$BLOOM_BITS"
    "${common_simfs_args_base[@]}"
    --simulate_xp_latency_ns=75
    "${db_shape_args[@]}"
    "${fill_extra_args[@]}"
    "${cache_args[@]}"
    --sample_probe_output="$sample_csv"
    --sample_probe_op=read
    --sample_probe_interval_ops="$CALIB_SAMPLE_INTERVAL_OPS"
    --sample_probe_max_samples="$CALIB_SAMPLE_MAX_SAMPLES"
    --sample_probe_case_label="cache${CACHE_SIZE}"
    --sample_probe_scenario="readmissing_sample"
  )
  printf "%q " "${read_cmd[@]}" >"$out_dir/02_readmissing_cache_${CACHE_SIZE}.cmd"
  printf "\n" >>"$out_dir/02_readmissing_cache_${CACHE_SIZE}.cmd"
  "${read_cmd[@]}" 2>&1 | tee "$out_dir/02_readmissing_cache_${CACHE_SIZE}.log"

  python3 "$ROOT_DIR/tools/analyze_tail_probe_samples.py" \
    --samples_csv "$sample_csv" \
    --out_dir "$analysis_dir" \
    --label "cache${CACHE_SIZE}" \
    --phase "calib_wbuf${wbuf}_tfile${tfile}_auto${disable_auto}" \
    --scenario "readmissing_sample" \
    --threshold_us 0 \
    --kind sample >/dev/null

  # Extract median probe across sample rows.
  local probe_stats
  probe_stats="$(
    python3 - "$analysis_dir/sample_stage_samples.csv" <<'PY'
import csv
import sys

p = sys.argv[1]
vals = []
with open(p, newline="", encoding="utf-8") as f:
    for r in csv.DictReader(f):
        try:
            vals.append(int(float(r.get("filter_probe_total", "0"))))
        except Exception:
            pass

if not vals:
    print("nan,0")
    raise SystemExit(0)

vals.sort()
med = vals[len(vals) // 2]
print(f"{med},{len(vals)}")
PY
  )"
  local probe_median_overall="${probe_stats%,*}"
  local samples="${probe_stats#*,}"
  echo "$wbuf,$tfile,$disable_auto,$probe_median_overall,$samples,$out_dir" >>"$calib_csv"

  echo "[exp46][calib] point done: wbuf=$wbuf tfile=$tfile auto=$disable_auto probe_median=$probe_median_overall samples=$samples"
}

if [[ "$SKIP_CALIBRATION" != "1" ]]; then
  # Task 1: calibration sweep (disable_auto_compactions=1).
  for i in "${!WBUF_ARRAY[@]}"; do
    wbuf="$(echo "${WBUF_ARRAY[$i]}" | tr -d ' ')"
    tfile="$(echo "${TFILE_ARRAY[$i]}" | tr -d ' ')"
    run_calibration_point "$wbuf" "$tfile" 1 "calib_wbuf${wbuf}_tfile${tfile}_auto1"
  done
fi

# Find a point in the target range. If none, optionally try Task 1b.
if [[ ! -f "$calib_csv" ]]; then
  echo "[exp46][fatal] calibration summary not found: $calib_csv" >&2
  echo "[exp46][fatal] run without SKIP_CALIBRATION=1 or point EXPERIMENT_DIR to an existing run dir" >&2
  exit 1
fi

chosen_line="$(
  python3 - "$calib_csv" <<'PY'
import csv
import sys

lo, hi = 10, 200
best = None

with open(sys.argv[1], newline="", encoding="utf-8") as f:
    rows = list(csv.DictReader(f))

for r in rows:
    try:
        p = float(r.get("probe_median_overall", "nan"))
    except Exception:
        continue
    if lo <= p <= hi:
        best = r
        break

if best is None:
    print("")
else:
    print(",".join([best["write_buffer_size"], best["target_file_size_base"], best["disable_auto_compactions"], best["run_dir"]]))
PY
)"

if [[ "$SKIP_CALIBRATION" != "1" && -z "$chosen_line" && "$ALLOW_LIMITED_COMPACTION_IF_NEEDED" == "1" ]]; then
  echo "[exp46] no calibration point reached probe 10..200; try limited compaction (Task 1b)"
  # Use the middle point as default for Task 1b.
  wbuf="$(echo "${WBUF_ARRAY[1]:-${WBUF_ARRAY[0]}}" | tr -d ' ')"
  tfile="$(echo "${TFILE_ARRAY[1]:-${TFILE_ARRAY[0]}}" | tr -d ' ')"
  run_calibration_point "$wbuf" "$tfile" 0 "calib_wbuf${wbuf}_tfile${tfile}_auto0"
  chosen_line="$(
    python3 - "$calib_csv" <<'PY'
import csv
import sys

lo, hi = 10, 200
best = None

with open(sys.argv[1], newline="", encoding="utf-8") as f:
    rows = list(csv.DictReader(f))

for r in rows:
    try:
        p = float(r.get("probe_median_overall", "nan"))
    except Exception:
        continue
    if lo <= p <= hi:
        best = r
        break

if best is None:
    print("")
else:
    print(",".join([best["write_buffer_size"], best["target_file_size_base"], best["disable_auto_compactions"], best["run_dir"]]))
PY
  )"
fi

if [[ -z "$chosen_line" ]]; then
  echo "[exp46][warn] no configuration reached probe 10..200. See: $calib_csv"
  exit 0
fi

IFS=',' read -r CHOSEN_WBUF CHOSEN_TFILE CHOSEN_AUTO CHOSEN_RUN_DIR <<<"$chosen_line"
echo "[exp46] chosen: wbuf=$CHOSEN_WBUF tfile=$CHOSEN_TFILE disable_auto_compactions=$CHOSEN_AUTO (run_dir=$CHOSEN_RUN_DIR)"

# Task 2: verification sweep at the chosen point (rebuild DB with chosen shape once).
safe_rmtree "$DB_DIR"
safe_rmtree "$WAL_DIR"
mkdir -p "$WAL_DIR"
if [[ "$USE_TMPFS_REDIRECT" == "1" ]]; then
  safe_rmtree "$TMPFS_ROOT"
fi

fill_extra_args=()
if [[ "$CHOSEN_AUTO" == "1" ]]; then
  fill_extra_args+=(--disable_auto_compactions=1)
fi

db_shape_args=(
  --key_size="$KEY_SIZE"
  --value_size="$VALUE_SIZE"
  --compression_type="$COMPRESSION_TYPE"
  --compression_ratio="$COMPRESSION_RATIO"
  --write_buffer_size="$CHOSEN_WBUF"
  --max_write_buffer_number=4
  --min_write_buffer_number_to_merge=1
  --target_file_size_base="$CHOSEN_TFILE"
  --max_bytes_for_level_base=268435456
  --level0_file_num_compaction_trigger=8
  --max_background_compactions=4
  --bloom_bits=10
)

echo "[exp46][sweep] fill DB for chosen point"
fill_cmd=(
  "$DB_BENCH"
  --db="$DB_DIR"
  --wal_dir="$WAL_DIR"
  --benchmarks="$FILL_BENCH",stats
  --statistics
  --num="$NUM_KEYS"
  --threads="$FILL_THREADS"
  "${common_simfs_args_base[@]}"
  --simulate_xp_latency_ns=750
  "${db_shape_args[@]}"
  "${fill_extra_args[@]}"
)
printf "%q " "${fill_cmd[@]}" >"$EXPERIMENT_DIR/10_fill_chosen.cmd"
printf "\n" >>"$EXPERIMENT_DIR/10_fill_chosen.cmd"
"${fill_cmd[@]}" 2>&1 | tee "$EXPERIMENT_DIR/10_fill_chosen.log"

if [[ "$CHOSEN_AUTO" != "1" ]]; then
  echo "[exp46][sweep] waitforcompaction (chosen point)"
  wait_cmd=(
    "$DB_BENCH"
    --db="$DB_DIR"
    --wal_dir="$WAL_DIR"
    --use_existing_db=1
    --benchmarks=waitforcompaction,stats
    --statistics
    "${common_simfs_args_base[@]}"
    --simulate_xp_latency_ns=750
    "${db_shape_args[@]}"
  )
  printf "%q " "${wait_cmd[@]}" >"$EXPERIMENT_DIR/11_waitforcompaction_chosen.cmd"
  printf "\n" >>"$EXPERIMENT_DIR/11_waitforcompaction_chosen.cmd"
  "${wait_cmd[@]}" 2>&1 | tee "$EXPERIMENT_DIR/11_waitforcompaction_chosen.log"
fi

points_csv="$EXPERIMENT_DIR/sweep_points.csv"
echo "run_tag,xp_latency_ns,bloom_bits,run_dir" >"$points_csv"

IFS=',' read -r -a XP_ARRAY <<<"$XP_LATENCY_LIST_NS"
for xp_ns in "${XP_ARRAY[@]}"; do
  xp_ns="$(echo "$xp_ns" | tr -d ' ')"
  run_tag="${EXP_DATE}_exp${GLOBAL_EXP_ID}_${EXPERIMENT_NAME}_verify_xplat${xp_ns}_${RUN_TIME}"
  out_dir="$EXPERIMENT_DIR/run_results/$run_tag"
  mkdir -p "$out_dir"

  sample_csv="$out_dir/02_readmissing_cache_${CACHE_SIZE}.sample_probe.csv"
  analysis_dir="$out_dir/analysis_readmissing_sample_cache_${CACHE_SIZE}"

  common_simfs_args=("${common_simfs_args_base[@]}" --simulate_xp_latency_ns="$xp_ns")

  read_cmd=(
    "$DB_BENCH"
    --db="$DB_DIR"
    --wal_dir="$WAL_DIR"
    --use_existing_db=1
    --benchmarks=readmissing,stats
    --statistics
    --num="$NUM_KEYS"
    --threads="$SWEEP_THREADS"
    --reads="$SWEEP_READS"
    --bloom_bits="$BLOOM_BITS"
    "${common_simfs_args[@]}"
    "${db_shape_args[@]}"
    "${fill_extra_args[@]}"
    "${cache_args[@]}"
    --sample_probe_output="$sample_csv"
    --sample_probe_op=read
    --sample_probe_interval_ops="$SWEEP_SAMPLE_INTERVAL_OPS"
    --sample_probe_max_samples="$SWEEP_SAMPLE_MAX_SAMPLES"
    --sample_probe_case_label="cache${CACHE_SIZE}"
    --sample_probe_scenario="readmissing_sample"
  )
  printf "%q " "${read_cmd[@]}" >"$out_dir/02_readmissing_cache_${CACHE_SIZE}.cmd"
  printf "\n" >>"$out_dir/02_readmissing_cache_${CACHE_SIZE}.cmd"
  "${read_cmd[@]}" 2>&1 | tee "$out_dir/02_readmissing_cache_${CACHE_SIZE}.log"

  python3 "$ROOT_DIR/tools/analyze_tail_probe_samples.py" \
    --samples_csv "$sample_csv" \
    --out_dir "$analysis_dir" \
    --label "cache${CACHE_SIZE}" \
    --phase "verify_xplat${xp_ns}" \
    --scenario "readmissing_sample" \
    --threshold_us 0 \
    --kind sample >/dev/null

  echo "$run_tag,$xp_ns,$BLOOM_BITS,$out_dir" >>"$points_csv"
done

python3 "$ROOT_DIR/tools/summarize_exp44_media_sweep.py" \
  --sweep_points_csv "$points_csv" \
  --out_csv "$EXPERIMENT_DIR/sweep_summary.csv" >/dev/null

echo "[exp46] done. calibration: $calib_csv"
echo "[exp46] done. sweep summary: $EXPERIMENT_DIR/sweep_summary.csv"
