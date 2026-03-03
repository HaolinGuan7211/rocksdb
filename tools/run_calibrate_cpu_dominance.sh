#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ ! -d "$ROOT_DIR/build" ]]; then
  echo "[calib] missing build/ (run cmake configure first)" >&2
  exit 1
fi

echo "[calib] building: cmake --build build"
cmake --build "$ROOT_DIR/build" -j"$(nproc)"

python3 "$ROOT_DIR/tools/calibrate_cpu_dominance.py" "$@"

