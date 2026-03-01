#!/usr/bin/env python3
"""
Replay an existing run_results/<RUN_TAG> command and collect periodically
sampled seek perf deltas (PerfContext/IOStatsContext) into a CSV file.

This complements TailProbe (latency-threshold-based) with an (approximately)
unbiased periodic sampler for "normal path" analysis.

Requires db_bench support for:
  --sample_probe_output
  --sample_probe_interval_ops
  --sample_probe_max_samples
  --sample_probe_case_label
  --sample_probe_scenario
"""

from __future__ import annotations

import argparse
import shlex
import subprocess
from pathlib import Path
from typing import List, Optional, Tuple


def _parse_cache_size_from_case_prefix(case_prefix: str) -> Optional[int]:
    # "02_mixgraph_cache_536870912" -> 536870912
    parts = case_prefix.split("_")
    if not parts:
        return None
    try:
        return int(parts[-1])
    except ValueError:
        return None


def _label_for_cache_size(cache_size: int) -> str:
    if cache_size == 0:
        return "cache0"
    if cache_size == 536870912:
        return "cache500m"
    if cache_size % (1024**3) == 0:
        return f"cache{cache_size // (1024**3)}g"
    return f"cache{cache_size}"


def _remove_flags(tokens: List[str], prefixes: Tuple[str, ...]) -> List[str]:
    out: List[str] = []
    i = 0
    while i < len(tokens):
        t = tokens[i]
        removed = False
        for p in prefixes:
            if t == p or t.startswith(p + "="):
                removed = True
                break
        if removed:
            i += 1
            continue
        if any(t == p for p in prefixes) and i + 1 < len(tokens):
            i += 2
            continue
        out.append(t)
        i += 1
    return out


def _ensure_min_perf_level(tokens: List[str], min_level: int) -> List[str]:
    found = False
    out: List[str] = []
    i = 0
    while i < len(tokens):
        t = tokens[i]
        if t.startswith("--perf_level="):
            found = True
            try:
                cur = int(t.split("=", 1)[1])
            except ValueError:
                cur = 0
            out.append(f"--perf_level={max(cur, min_level)}")
            i += 1
            continue
        if t == "--perf_level" and i + 1 < len(tokens):
            found = True
            try:
                cur = int(tokens[i + 1])
            except ValueError:
                cur = 0
            out.append("--perf_level")
            out.append(str(max(cur, min_level)))
            i += 2
            continue
        out.append(t)
        i += 1
    if not found:
        out.append(f"--perf_level={min_level}")
    return out


def _run_and_log(tokens: List[str], log_path: Path, cwd: Path) -> int:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8") as f:
        f.write("# cmd: " + " ".join(shlex.quote(t) for t in tokens) + "\n")
        f.flush()
        proc = subprocess.run(tokens, cwd=str(cwd), stdout=f, stderr=subprocess.STDOUT, check=False)
        return proc.returncode


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--run_dir", required=True, help="run_results/<RUN_TAG> dir")
    ap.add_argument("--out_subdir", default="sample_probe", help="subdir under run_dir to write artifacts")
    ap.add_argument("--phase", default="sample", help="phase name used in output filenames")
    ap.add_argument("--interval_seek_ops", type=int, required=True, help="sample one seek every N seek ops per thread")
    ap.add_argument("--max_samples", type=int, default=50000, help="max rows per db_bench process")
    ap.add_argument("--perf_level", type=int, default=6, help="minimum perf_level for replay")
    ap.add_argument("--reuse_existing_samples", action="store_true", help="skip rerun if samples exist")
    args = ap.parse_args()

    run_dir = Path(args.run_dir).resolve()
    if not run_dir.exists():
        raise SystemExit(f"run_dir not found: {run_dir}")

    out_root = (run_dir / args.out_subdir).resolve()
    samples_dir = out_root / "samples"
    logs_dir = out_root / "run_logs"
    out_root.mkdir(parents=True, exist_ok=True)
    samples_dir.mkdir(parents=True, exist_ok=True)
    logs_dir.mkdir(parents=True, exist_ok=True)

    cmd_files = sorted(run_dir.glob("02_mixgraph_cache_*.cmd"))
    if not cmd_files:
        raise SystemExit(f"no mixgraph cmd files found under: {run_dir}")

    for cmd_path in cmd_files:
        case_prefix = cmd_path.name.replace(".cmd", "")
        cache_size = _parse_cache_size_from_case_prefix(case_prefix) or 0
        label = _label_for_cache_size(cache_size)
        phase = str(args.phase)

        sample_path = (samples_dir / f"{label}_{phase}_samples.csv").resolve()
        log_path = (logs_dir / f"{case_prefix}.sample_probe.log").resolve()

        if args.reuse_existing_samples and sample_path.exists():
            continue

        tokens = shlex.split(cmd_path.read_text(encoding="utf-8", errors="ignore").strip())
        if not tokens:
            raise SystemExit(f"empty cmd file: {cmd_path}")

        # Avoid overwriting baseline monitoring outputs.
        tokens = _remove_flags(
            tokens,
            prefixes=(
                "--mix_monitor_cache_csv",
                "--mix_monitor_stage_csv",
                "--mix_monitor_events_csv",
                "--mix_monitor_enable",
                "--mix_monitor_window_us",
                "--mix_probe_enable",
                "--mix_probe_interval_ops",
                "--mix_probe_reads",
                "--simulate_xp_monitor_enable",
                "--simulate_xp_monitor_window_us",
                "--simulate_xp_monitor_stage_seconds",
                "--simulate_xp_monitor_max_read",
                "--simulate_xp_monitor_max_open",
                "--simulate_xp_monitor_max_prefetch",
                "--simulate_xp_monitor_window_csv",
                "--simulate_xp_monitor_stage_csv",
                "--tail_probe_output",
                "--tail_probe_threshold_us",
                "--tail_probe_max_samples",
                "--tail_probe_case_label",
                "--tail_probe_scenario",
                "--sample_probe_output",
                "--sample_probe_interval_ops",
                "--sample_probe_max_samples",
                "--sample_probe_case_label",
                "--sample_probe_scenario",
            ),
        )

        tokens = _ensure_min_perf_level(tokens, min_level=int(args.perf_level))
        tokens.append(f"--sample_probe_output={sample_path}")
        tokens.append(f"--sample_probe_interval_ops={int(args.interval_seek_ops)}")
        tokens.append(f"--sample_probe_max_samples={int(args.max_samples)}")
        tokens.append(f"--sample_probe_case_label={label}")
        tokens.append("--sample_probe_scenario=mixgraph_sample")

        rc = _run_and_log(tokens, log_path, cwd=Path.cwd())
        if rc != 0:
            raise SystemExit(f"sample-probe run failed rc={rc}, see {log_path}")

        print(f"[ok] wrote samples: {sample_path}")
    print(f"[ok] artifacts under: {out_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

