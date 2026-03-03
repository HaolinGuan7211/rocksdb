#!/usr/bin/env python3
"""
Tail-probe runner + analyzer for an existing run_results/<RUN_TAG> directory.

This is a lightweight alternative to run_tail_probe_from_matrix.py for ad-hoc
experiments (e.g. exp34 cache0 vs cache500m).

Inputs (under --run_dir):
  - 02_mixgraph_cache_*.cmd (db_bench invocation)
  - 02_mixgraph_cache_*.log (baseline run output; used to choose threshold)
  - monitor_analysis/02_mixgraph_cache_*.op_latency_percentiles.csv (preferred)

Outputs (under <run_dir>/<out_subdir>/):
  - samples/<label>_cache_sweep_tail_samples.csv (TailProbeWriter raw samples)
  - tail_threshold_summary.csv
  - tail_stage_samples.csv
  - tail_stage_breakdown.csv
  - tail_stage_distribution.csv
  - tail_latency_bucket_breakdown.csv
  - phaseA_tail_seek_stage_stack.png
  - phaseA_tail_component_share_distribution.png
  - phaseA_tail_latency_bucket_stage_stack.png
  - tail_probe_report.md
"""

from __future__ import annotations

import argparse
import csv
import math
import shlex
import subprocess
from pathlib import Path
from typing import Dict, List, Optional, Tuple


def _read_csv(path: Path) -> List[Dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def _write_csv(path: Path, rows: List[Dict[str, object]]) -> None:
    if not rows:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    keys = list(rows[0].keys())
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        for r in rows:
            w.writerow(r)


def _safe_float(v: object) -> float:
    if v is None:
        return float("nan")
    s = str(v).strip()
    if not s:
        return float("nan")
    try:
        return float(s)
    except ValueError:
        return float("nan")


def _is_finite(x: float) -> bool:
    return not math.isnan(x) and not math.isinf(x)


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
        # Handle "--flag value" style for known prefixes.
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


def _remove_flag_with_optional_value(tokens: List[str], flag: str) -> List[str]:
    """
    Remove a CLI flag in either form:
      --flag
      --flag=value
      --flag value
    """
    out: List[str] = []
    i = 0
    while i < len(tokens):
        t = tokens[i]
        if t == flag:
            # Drop this token and its argument (if present).
            i += 2
            continue
        if t.startswith(flag + "="):
            i += 1
            continue
        out.append(t)
        i += 1
    return out


def _override_kv_flag(tokens: List[str], flag: str, value: object) -> List[str]:
    tokens = _remove_flag_with_optional_value(tokens, flag)
    tokens.append(f"{flag}={value}")
    return tokens


def _read_seek_p99_from_op_percentiles(op_csv: Path) -> Optional[float]:
    if not op_csv.exists():
        return None
    rows = _read_csv(op_csv)
    for r in rows:
        if (r.get("op", "") or "").strip().lower() == "seek":
            v = _safe_float(r.get("p99_us", ""))
            if _is_finite(v) and v > 0:
                return v
    return None


def _read_op_p99_from_op_percentiles(op_csv: Path, op_name: str) -> Optional[float]:
    # op_latency_percentiles.csv format: op,p50_us,p95_us,p99_us
    # It usually contains both fine-grained (Get/MultiGet/Seek) and aggregated
    # (read/seek) rows. We prefer aggregated rows for tail-probe thresholding.
    with op_csv.open("r", encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            if str(r.get("op", "")).strip() != op_name:
                continue
            v = _safe_float(r.get("p99_us", ""))
            if _is_finite(v) and v > 0:
                return v
    return None


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
    ap.add_argument("--out_subdir", default="tail_probe", help="subdir under run_dir to write artifacts")
    ap.add_argument("--phase", default="cache_sweep", help="phase name used in reports (default: cache_sweep)")
    ap.add_argument("--p99_multiplier", type=float, default=1.0, help="threshold_us = max(min, p99*multiplier)")
    ap.add_argument("--min_threshold_us", type=int, default=2000, help="minimum threshold (us)")
    ap.add_argument(
        "--op",
        default="seek",
        choices=("seek", "read"),
        help="which op type to probe: seek (kSeek) or read (kRead: Get/MultiGet)",
    )
    ap.add_argument("--max_samples", type=int, default=20000, help="tail probe max samples")
    ap.add_argument("--perf_level", type=int, default=4, help="min perf_level to enable perf_context timing")
    ap.add_argument(
        "--duration_override_seconds",
        type=int,
        default=0,
        help="if >0, override --duration used for the tail-probe rerun (baseline run remains unchanged)",
    )
    ap.add_argument("--reuse_existing_samples", action="store_true", help="skip rerunning db_bench if samples exist")
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

    # Import the analyzer module (reuse its stage reconstruction + plots).
    import sys

    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import run_tail_probe_from_matrix as tpm  # type: ignore

    cmd_files = sorted(run_dir.glob("02_mixgraph_cache_*.cmd"))
    if not cmd_files:
        raise SystemExit(f"no mixgraph cmd files found under: {run_dir}")

    threshold_rows: List[Dict[str, object]] = []
    all_sample_rows: List[Dict[str, object]] = []

    for cmd_path in cmd_files:
        case_prefix = cmd_path.name.replace(".cmd", "")
        cache_size = _parse_cache_size_from_case_prefix(case_prefix) or 0
        label = _label_for_cache_size(cache_size)
        phase = str(args.phase)

        op_csv = run_dir / "monitor_analysis" / f"{case_prefix}.op_latency_percentiles.csv"
        op_name = "seek" if str(args.op) == "seek" else "read"
        op_p99_us = _read_op_p99_from_op_percentiles(op_csv, op_name)
        if op_p99_us is None:
            raise SystemExit(f"missing {op_name} p99 (need {op_csv})")

        threshold_us = max(args.min_threshold_us, int(op_p99_us * float(args.p99_multiplier)))

        sample_path = (samples_dir / f"{label}_{phase}_tail_samples.csv").resolve()
        log_path = (logs_dir / f"{case_prefix}.tail_probe.log").resolve()

        if not args.reuse_existing_samples or not sample_path.exists():
            if sample_path.exists():
                sample_path.unlink()
            tokens = shlex.split(cmd_path.read_text(encoding="utf-8", errors="ignore").strip())
            if not tokens:
                raise SystemExit(f"empty cmd file: {cmd_path}")

            # Avoid overwriting the baseline monitoring CSVs; tail-probe only needs perf/iostats.
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
                ),
            )

            tokens = _ensure_min_perf_level(tokens, min_level=int(args.perf_level))
            if int(args.duration_override_seconds) > 0:
                tokens = _override_kv_flag(tokens, "--duration", int(args.duration_override_seconds))
            tokens.append(f"--tail_probe_output={sample_path}")
            tokens.append(f"--tail_probe_threshold_us={threshold_us}")
            tokens.append(f"--tail_probe_max_samples={int(args.max_samples)}")
            tokens.append(f"--tail_probe_case_label={label}")
            tokens.append(f"--tail_probe_op={args.op}")
            tokens.append("--tail_probe_scenario=mixgraph")

            rc = _run_and_log(tokens, log_path, cwd=Path.cwd())
            if rc != 0:
                raise SystemExit(f"tail-probe run failed rc={rc}, see {log_path}")

        tail_rows = tpm.read_csv(sample_path)
        sample_count = len(tail_rows)

        threshold_rows.append(
            {
                "label": label,
                "phase": phase,
                "case_prefix": case_prefix,
                "cache_size": cache_size,
                "op": str(args.op),
                "op_p99_us": op_p99_us,
                "threshold_us": threshold_us,
                "p99_multiplier": float(args.p99_multiplier),
                "min_threshold_us": int(args.min_threshold_us),
                "samples": sample_count,
                "max_samples": int(args.max_samples),
                "samples_file": str(sample_path),
                "log_file": str(log_path),
            }
        )

        for tr in tail_rows:
            all_sample_rows.append(
                tpm.build_sample_row(
                    label=label,
                    phase=phase,
                    scenario="mixgraph",
                    threshold_us=threshold_us,
                    tr=tr,
                )
            )

    # Write required artifacts (aligned with workflow spec).
    _write_csv(out_root / "tail_threshold_summary.csv", threshold_rows)
    _write_csv(out_root / "tail_stage_samples.csv", all_sample_rows)

    breakdown_rows = tpm.build_breakdown_rows(all_sample_rows)
    _write_csv(out_root / "tail_stage_breakdown.csv", breakdown_rows)

    dist_rows = tpm.build_stage_distribution_rows(all_sample_rows)
    _write_csv(out_root / "tail_stage_distribution.csv", dist_rows)

    bucket_rows = tpm.build_latency_bucket_breakdown_rows(all_sample_rows)
    _write_csv(out_root / "tail_latency_bucket_breakdown.csv", bucket_rows)

    # Plots (use the same naming convention as the spec: phaseA_* for cache_sweep).
    prefix = tpm.phase_to_output_prefix(str(args.phase))
    tpm.plot_phase_stack(
        breakdown_rows,
        phase=str(args.phase),
        out_png=out_root / f"phase{prefix}_tail_seek_stage_stack.png",
    )
    tpm.plot_phase_component_share_distribution(
        all_sample_rows,
        phase=str(args.phase),
        out_png=out_root / f"phase{prefix}_tail_component_share_distribution.png",
    )
    tpm.plot_phase_latency_bucket_stage_stack(
        bucket_rows,
        phase=str(args.phase),
        out_png=out_root / f"phase{prefix}_tail_latency_bucket_stage_stack.png",
    )

    # Filter-centric plots (paper-style signals). Best-effort: only runs if the
    # sample rows contain the computed fields.
    try:
        tpm.plot_phase_filter_total_share_distribution(
            all_sample_rows,
            phase=str(args.phase),
            out_png=out_root / f"phase{prefix}_tail_filter_total_share_distribution.png",
        )
        tpm.plot_phase_filter_probe_vs_share_scatter(
            all_sample_rows,
            phase=str(args.phase),
            out_png=out_root / f"phase{prefix}_tail_filter_probe_vs_share_scatter.png",
        )
        probe_summary_rows = tpm.build_filter_probe_bucket_summary_rows(
            all_sample_rows, phase=str(args.phase)
        )
        if probe_summary_rows:
            _write_csv(out_root / "tail_filter_probe_bucket_summary.csv", probe_summary_rows)
            tpm.plot_phase_filter_probe_knee_latency(
                probe_summary_rows,
                phase=str(args.phase),
                out_png=out_root / f"phase{prefix}_tail_filter_probe_knee_latency.png",
            )
            tpm.plot_phase_filter_probe_knee_filter_share(
                probe_summary_rows,
                phase=str(args.phase),
                out_png=out_root / f"phase{prefix}_tail_filter_probe_knee_filter_share.png",
            )
    except Exception:
        pass

    # Short report.
    report = out_root / "tail_probe_report.md"
    lines: List[str] = []
    lines.append("# Tail Probe Report\n")
    lines.append(f"- run_dir: `{run_dir}`\n")
    lines.append(f"- phase: `{args.phase}`\n")
    lines.append(f"- op: `{args.op}`\n")
    lines.append(f"- p99_multiplier: `{args.p99_multiplier}`\n")
    lines.append(f"- min_threshold_us: `{args.min_threshold_us}`\n")
    lines.append(f"- max_samples: `{args.max_samples}`\n")
    lines.append("\n## Thresholds\n")
    for r in threshold_rows:
        lines.append(
            f"- {r['label']}: op_p99_us={r['op_p99_us']:.3f}, "
            f"threshold_us={r['threshold_us']}, samples={r['samples']}\n"
        )
    lines.append("\n## Artifacts\n")
    for p in [
        "tail_threshold_summary.csv",
        "tail_stage_samples.csv",
        "tail_stage_breakdown.csv",
        "tail_stage_distribution.csv",
        "tail_latency_bucket_breakdown.csv",
        "tail_filter_probe_bucket_summary.csv",
        f"phase{prefix}_tail_seek_stage_stack.png",
        f"phase{prefix}_tail_component_share_distribution.png",
        f"phase{prefix}_tail_latency_bucket_stage_stack.png",
        f"phase{prefix}_tail_filter_total_share_distribution.png",
        f"phase{prefix}_tail_filter_probe_vs_share_scatter.png",
        f"phase{prefix}_tail_filter_probe_knee_latency.png",
        f"phase{prefix}_tail_filter_probe_knee_filter_share.png",
    ]:
        lines.append(f"- `{out_root / p}`\n")
    report.write_text("".join(lines), encoding="utf-8")

    print(f"[ok] wrote tail probe artifacts under: {out_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
