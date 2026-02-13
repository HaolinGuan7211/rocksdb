#!/usr/bin/env python3
import argparse
import csv
import math
import re
from pathlib import Path
from typing import Dict, List, Optional, Tuple


RESULT_RE = re.compile(
    r"^([A-Za-z0-9_]+)\s+:\s+([0-9.]+)\s+micros/op\s+([0-9]+)\s+ops/sec(?:.*?([0-9.]+)\s+MB/s)?"
)


def parse_kv_file(path: Path) -> Dict[str, float]:
    out: Dict[str, float] = {}
    if not path.is_file():
        return out
    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        k = k.strip()
        v = v.strip()
        if not k:
            continue
        try:
            out[k] = float(v)
        except ValueError:
            continue
    return out


def parse_db_bench_result(path: Path) -> Dict[str, float]:
    out: Dict[str, float] = {}
    if not path.is_file():
        return out
    last: Optional[Tuple[str, str, str, str]] = None
    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        m = RESULT_RE.match(line.strip())
        if m:
            last = (m.group(1), m.group(2), m.group(3), m.group(4) or "0")
    if last is None:
        return out
    out["benchmark"] = 0.0  # placeholder to indicate success
    out["micros_per_op"] = float(last[1])
    out["ops_per_sec"] = float(last[2])
    out["mb_per_sec"] = float(last[3])
    return out


def safe_div(a: float, b: float) -> float:
    if b == 0:
        return 0.0
    return a / b


def load_reference_csv(path: Path) -> List[Dict[str, str]]:
    if not path.is_file():
        return []
    with path.open("r", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def main() -> int:
    ap = argparse.ArgumentParser(description="Analyze env fs XP simulation bench.")
    ap.add_argument("--run-dir", required=True)
    ap.add_argument("--reference-csv", default="")
    args = ap.parse_args()

    run_dir = Path(args.run_dir).resolve()
    manifest = run_dir / "case_manifest.csv"
    if not manifest.is_file():
        raise SystemExit(f"missing manifest: {manifest}")

    analysis_dir = run_dir / "analysis"
    analysis_dir.mkdir(parents=True, exist_ok=True)
    summary_csv = analysis_dir / "envfs_xp_summary.csv"
    report_md = analysis_dir / "envfs_xp_report.md"

    rows: List[Dict[str, object]] = []
    with manifest.open("r", encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            log_path = run_dir / str(r.get("log_file", ""))
            stats_path = run_dir / str(r.get("stats_file", ""))

            dbm = parse_db_bench_result(log_path)
            sm = parse_kv_file(stats_path)

            logical_read = float(sm.get("logical_read_bytes", 0.0))
            logical_write = float(sm.get("logical_write_bytes", 0.0))
            media_read = float(sm.get("media_read_bytes", 0.0))
            media_write = float(sm.get("media_write_bytes", 0.0))
            read_ops = float(sm.get("read_ops", 0.0))
            write_ops = float(sm.get("write_ops", 0.0))
            read_delay = float(sm.get("simulated_read_delay_ns", 0.0))
            write_delay = float(sm.get("simulated_write_delay_ns", 0.0))
            read_queue_delay = float(sm.get("simulated_read_queue_delay_ns", 0.0))
            read_media_delay = float(sm.get("simulated_read_media_delay_ns", 0.0))
            write_queue_delay = float(sm.get("simulated_write_queue_delay_ns", 0.0))
            write_media_delay = float(sm.get("simulated_write_media_delay_ns", 0.0))
            read_prefetch_hits = float(sm.get("read_prefetch_hits", 0.0))
            line_bytes = float(sm.get("xp_line_bytes", 0.0))

            sim_read_lat = safe_div(read_delay, read_ops)
            sim_write_lat = safe_div(write_delay, write_ops)
            sim_read_queue_lat = safe_div(read_queue_delay, read_ops)
            sim_read_media_lat = safe_div(read_media_delay, read_ops)
            sim_write_queue_lat = safe_div(write_queue_delay, write_ops)
            sim_write_media_lat = safe_div(write_media_delay, write_ops)
            sim_read_bw = safe_div(media_read, safe_div(read_delay, 1e9)) / (
                1024.0 * 1024.0
            ) if read_delay > 0 else 0.0
            sim_write_bw = safe_div(media_write, safe_div(write_delay, 1e9)) / (
                1024.0 * 1024.0
            ) if write_delay > 0 else 0.0
            ewr = safe_div(media_write, logical_write)
            prefetch_hit_ratio = safe_div(read_prefetch_hits, read_ops)
            ns_per_line_read = (
                safe_div(read_media_delay, safe_div(media_read, line_bytes))
                if line_bytes > 0 and media_read > 0
                else 0.0
            )
            ns_per_line_write = (
                safe_div(write_media_delay, safe_div(media_write, line_bytes))
                if line_bytes > 0 and media_write > 0
                else 0.0
            )

            row: Dict[str, object] = {
                "mode": r.get("mode", ""),
                "case": r.get("case", ""),
                "case_id": f"{r.get('mode', '')}_{r.get('case', '')}",
                "benchmark": r.get("benchmark", ""),
                "threads": int(r.get("threads", "0") or 0),
                "value_size": int(r.get("value_size", "0") or 0),
                "micros_per_op": float(dbm.get("micros_per_op", 0.0)),
                "ops_per_sec": float(dbm.get("ops_per_sec", 0.0)),
                "mb_per_sec": float(dbm.get("mb_per_sec", 0.0)),
                "logical_read_bytes": logical_read,
                "logical_write_bytes": logical_write,
                "media_read_bytes": media_read,
                "media_write_bytes": media_write,
                "read_ops": read_ops,
                "write_ops": write_ops,
                "simulated_read_delay_ns": read_delay,
                "simulated_write_delay_ns": write_delay,
                "simulated_read_queue_delay_ns": read_queue_delay,
                "simulated_read_media_delay_ns": read_media_delay,
                "simulated_write_queue_delay_ns": write_queue_delay,
                "simulated_write_media_delay_ns": write_media_delay,
                "read_prefetch_hits": read_prefetch_hits,
                "sim_read_latency_ns_per_op": sim_read_lat,
                "sim_write_latency_ns_per_op": sim_write_lat,
                "sim_read_queue_latency_ns_per_op": sim_read_queue_lat,
                "sim_read_media_latency_ns_per_op": sim_read_media_lat,
                "sim_write_queue_latency_ns_per_op": sim_write_queue_lat,
                "sim_write_media_latency_ns_per_op": sim_write_media_lat,
                "sim_read_bw_mib_s": sim_read_bw,
                "sim_write_bw_mib_s": sim_write_bw,
                "ns_per_line_read": ns_per_line_read,
                "ns_per_line_write": ns_per_line_write,
                "prefetch_hit_ratio": prefetch_hit_ratio,
                "ewr": ewr,
            }
            rows.append(row)

    fields = [
        "mode",
        "case",
        "case_id",
        "benchmark",
        "threads",
        "value_size",
        "micros_per_op",
        "ops_per_sec",
        "mb_per_sec",
        "logical_read_bytes",
        "logical_write_bytes",
        "media_read_bytes",
        "media_write_bytes",
        "read_ops",
        "write_ops",
        "simulated_read_delay_ns",
        "simulated_write_delay_ns",
        "simulated_read_queue_delay_ns",
        "simulated_read_media_delay_ns",
        "simulated_write_queue_delay_ns",
        "simulated_write_media_delay_ns",
        "read_prefetch_hits",
        "sim_read_latency_ns_per_op",
        "sim_write_latency_ns_per_op",
        "sim_read_queue_latency_ns_per_op",
        "sim_read_media_latency_ns_per_op",
        "sim_write_queue_latency_ns_per_op",
        "sim_write_media_latency_ns_per_op",
        "sim_read_bw_mib_s",
        "sim_write_bw_mib_s",
        "ns_per_line_read",
        "ns_per_line_write",
        "prefetch_hit_ratio",
        "ewr",
    ]
    with summary_csv.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for row in rows:
            w.writerow(row)

    checks: List[Dict[str, object]] = []
    if args.reference_csv:
        refs = load_reference_csv(Path(args.reference_csv))
        by_case = {str(r["case_id"]): r for r in rows}
        for ref in refs:
            case = ref.get("case", "")
            metric = ref.get("metric", "")
            target = float(ref.get("target", "0") or 0)
            tol_pct = float(ref.get("tolerance_pct", "0") or 0)
            hit = by_case.get(case)
            if hit is None:
                checks.append(
                    {
                        "case": case,
                        "metric": metric,
                        "target": target,
                        "actual": math.nan,
                        "error_pct": math.nan,
                        "tolerance_pct": tol_pct,
                        "pass": False,
                    }
                )
                continue
            actual = float(hit.get(metric, 0.0))
            error_pct = abs(actual - target) * 100.0 / target if target != 0 else 0.0
            ok = error_pct <= tol_pct
            checks.append(
                {
                    "case": case,
                    "metric": metric,
                    "target": target,
                    "actual": actual,
                    "error_pct": error_pct,
                    "tolerance_pct": tol_pct,
                    "pass": ok,
                }
            )

    lines: List[str] = []
    lines.append("# Env FS XP Simulation Report")
    lines.append("")
    lines.append(f"- run_dir: `{run_dir}`")
    lines.append(f"- summary_csv: `{summary_csv}`")
    lines.append("")
    lines.append("## Core Metrics")
    lines.append(
        "| case_id | benchmark | threads | value_size | db_micros/op | db_ops/sec | db_MB/s | sim_read_lat(ns/op) | read_q(ns/op) | read_media(ns/op) | sim_write_lat(ns/op) | write_q(ns/op) | write_media(ns/op) | sim_read_bw(MiB/s) | sim_write_bw(MiB/s) | prefetch_hit_ratio | ewr |"
    )
    lines.append(
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
    )
    for r in rows:
        lines.append(
            "| {case_id} | {benchmark} | {threads} | {value_size} | {micros_per_op:.3f} | {ops_per_sec:.0f} | {mb_per_sec:.3f} | {sim_read_latency_ns_per_op:.3f} | {sim_read_queue_latency_ns_per_op:.3f} | {sim_read_media_latency_ns_per_op:.3f} | {sim_write_latency_ns_per_op:.3f} | {sim_write_queue_latency_ns_per_op:.3f} | {sim_write_media_latency_ns_per_op:.3f} | {sim_read_bw_mib_s:.3f} | {sim_write_bw_mib_s:.3f} | {prefetch_hit_ratio:.6f} | {ewr:.6f} |".format(
                **r
            )
        )

    if checks:
        passed = sum(1 for c in checks if c["pass"])
        lines.append("")
        lines.append("## Reference Check")
        lines.append(f"- passed: {passed}/{len(checks)}")
        lines.append(
            "| case | metric | target | actual | error_pct | tolerance_pct | pass |"
        )
        lines.append("|---|---|---:|---:|---:|---:|---|")
        for c in checks:
            actual_str = "NA" if math.isnan(float(c["actual"])) else f"{float(c['actual']):.6f}"
            err_str = "NA" if math.isnan(float(c["error_pct"])) else f"{float(c['error_pct']):.4f}"
            lines.append(
                f"| {c['case']} | {c['metric']} | {float(c['target']):.6f} | {actual_str} | {err_str} | {float(c['tolerance_pct']):.4f} | {str(c['pass']).lower()} |"
            )

    report_md.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"summary: {summary_csv}")
    print(f"report: {report_md}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
