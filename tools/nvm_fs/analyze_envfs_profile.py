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


def parse_bench_result(path: Path) -> Dict[str, float]:
    out: Dict[str, float] = {}
    if not path.is_file():
        return out
    results: Dict[str, Tuple[float, float, float]] = {}
    last_name: Optional[str] = None
    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        m = RESULT_RE.match(line.strip())
        if m:
            name = m.group(1)
            micros = float(m.group(2))
            ops = float(m.group(3))
            mb = float(m.group(4) or "0")
            results[name] = (micros, ops, mb)
            last_name = name
    if not results:
        return out

    # Keep backward compatibility:
    # - db_bench style: only one benchmark line
    # - envfs_profile_tool: emits envfs_wall and envfs_sim
    # Prefer envfs_sim for model fitting, then fallback.
    if "envfs_sim" in results:
        preferred_name = "envfs_sim"
    elif "envfs_wall" in results:
        preferred_name = "envfs_wall"
    else:
        preferred_name = last_name
    if preferred_name is None:
        return out
    preferred = results[preferred_name]
    out["micros_per_op"] = preferred[0]
    out["ops_per_sec"] = preferred[1]
    out["mb_per_sec"] = preferred[2]
    out["gb_per_sec"] = preferred[2] / 1000.0

    if "envfs_wall" in results:
        wall = results["envfs_wall"]
        out["wall_micros_per_op"] = wall[0]
        out["wall_ops_per_sec"] = wall[1]
        out["wall_mb_per_sec"] = wall[2]
        out["wall_gb_per_sec"] = wall[2] / 1000.0
    if "envfs_sim" in results:
        sim = results["envfs_sim"]
        out["sim_micros_per_op"] = sim[0]
        out["sim_ops_per_sec"] = sim[1]
        out["sim_mb_per_sec"] = sim[2]
        out["sim_gb_per_sec"] = sim[2] / 1000.0
    return out


def safe_div(a: float, b: float) -> float:
    if b == 0:
        return 0.0
    return a / b


def append_anchor(
    out: List[Dict[str, object]],
    anchor: str,
    metric: str,
    target: float,
    actual: float,
    tolerance_pct: float,
) -> None:
    if math.isnan(actual):
        error_pct = math.nan
        passed = False
    elif target == 0:
        error_pct = 0.0
        passed = abs(actual) <= 1e-12
    else:
        error_pct = abs(actual - target) * 100.0 / abs(target)
        passed = error_pct <= tolerance_pct
    out.append(
        {
            "anchor": anchor,
            "metric": metric,
            "target": target,
            "actual": actual,
            "error_pct": error_pct,
            "tolerance_pct": tolerance_pct,
            "pass": passed,
        }
    )


def find_peak(rows: List[Dict[str, object]], prefix: str) -> Tuple[float, int]:
    picks = [r for r in rows if str(r.get("case_id", "")).startswith(prefix)]
    if not picks:
        return (math.nan, -1)
    best = max(picks, key=lambda r: float(r.get("mb_per_sec", 0.0)))
    return (float(best.get("mb_per_sec", 0.0)), int(best.get("threads", 0)))


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Analyze env_fs profile run against simulated_doc anchors."
    )
    ap.add_argument("--run-dir", required=True)
    args = ap.parse_args()

    run_dir = Path(args.run_dir).resolve()
    manifest = run_dir / "case_manifest.csv"
    if not manifest.is_file():
        raise SystemExit(f"missing manifest: {manifest}")

    analysis_dir = run_dir / "analysis"
    analysis_dir.mkdir(parents=True, exist_ok=True)
    summary_csv = analysis_dir / "envfs_profile_summary.csv"
    anchors_csv = analysis_dir / "envfs_profile_anchors.csv"
    report_md = analysis_dir / "envfs_profile_report.md"

    rows: List[Dict[str, object]] = []
    with manifest.open("r", encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            case_id = str(r.get("case_id", ""))
            log_path = run_dir / str(r.get("log_file", ""))
            stats_path = run_dir / str(r.get("stats_file", ""))

            dbm = parse_bench_result(log_path)
            sm = parse_kv_file(stats_path)

            logical_read = float(sm.get("logical_read_bytes", 0.0))
            logical_write = float(sm.get("logical_write_bytes", 0.0))
            media_read = float(sm.get("media_read_bytes", 0.0))
            media_write = float(sm.get("media_write_bytes", 0.0))
            read_ops = float(sm.get("read_ops", 0.0))
            write_ops = float(sm.get("write_ops", 0.0))
            read_delay = float(sm.get("simulated_read_delay_ns", 0.0))
            write_delay = float(sm.get("simulated_write_delay_ns", 0.0))
            read_fixed_delay = float(sm.get("simulated_read_fixed_delay_ns", 0.0))
            write_fixed_delay = float(sm.get("simulated_write_fixed_delay_ns", 0.0))
            read_queue_delay = float(sm.get("simulated_read_queue_delay_ns", 0.0))
            read_media_delay = float(sm.get("simulated_read_media_delay_ns", 0.0))
            read_arb_delay = float(sm.get("simulated_read_arb_delay_ns", 0.0))
            write_queue_delay = float(sm.get("simulated_write_queue_delay_ns", 0.0))
            write_media_delay = float(sm.get("simulated_write_media_delay_ns", 0.0))
            read_prefetch_hits = float(sm.get("read_prefetch_hits", 0.0))
            line_bytes = float(sm.get("xp_line_bytes", 0.0))

            sim_read_lat = safe_div(read_delay, read_ops)
            sim_write_lat = safe_div(write_delay, write_ops)
            sim_read_fixed_lat = safe_div(read_fixed_delay, read_ops)
            sim_write_fixed_lat = safe_div(write_fixed_delay, write_ops)
            sim_read_queue_lat = safe_div(read_queue_delay, read_ops)
            sim_read_media_lat = safe_div(read_media_delay, read_ops)
            sim_read_arb_lat = safe_div(read_arb_delay, read_ops)
            sim_write_queue_lat = safe_div(write_queue_delay, write_ops)
            sim_write_media_lat = safe_div(write_media_delay, write_ops)
            sim_read_bw = (
                safe_div(media_read, safe_div(read_delay, 1e9)) / (1024.0 * 1024.0)
                if read_delay > 0
                else 0.0
            )
            sim_write_bw = (
                safe_div(media_write, safe_div(write_delay, 1e9)) / (1024.0 * 1024.0)
                if write_delay > 0
                else 0.0
            )
            # paper EWR: logical / media (<=1 means write amplification exists)
            ewr_paper = safe_div(logical_write, media_write)
            write_amp = safe_div(media_write, logical_write)
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
                "case_id": case_id,
                "group": r.get("group", ""),
                "op": r.get("op", ""),
                "pattern": r.get("pattern", ""),
                "location": r.get("location", ""),
                "mode": r.get("mode", ""),
                "benchmark": r.get("benchmark", ""),
                "threads": int(r.get("threads", "0") or 0),
                "value_size": int(r.get("value_size", "0") or 0),
                "num": int(r.get("num", "0") or 0),
                "reads": int(r.get("reads", "0") or 0),
                "micros_per_op": float(dbm.get("micros_per_op", 0.0)),
                "ops_per_sec": float(dbm.get("ops_per_sec", 0.0)),
                "mb_per_sec": float(dbm.get("mb_per_sec", 0.0)),
                "gb_per_sec": float(dbm.get("gb_per_sec", 0.0)),
                "wall_micros_per_op": float(dbm.get("wall_micros_per_op", 0.0)),
                "wall_ops_per_sec": float(dbm.get("wall_ops_per_sec", 0.0)),
                "wall_mb_per_sec": float(dbm.get("wall_mb_per_sec", 0.0)),
                "wall_gb_per_sec": float(dbm.get("wall_gb_per_sec", 0.0)),
                "sim_micros_per_op": float(dbm.get("sim_micros_per_op", 0.0)),
                "sim_ops_per_sec": float(dbm.get("sim_ops_per_sec", 0.0)),
                "sim_mb_per_sec": float(dbm.get("sim_mb_per_sec", 0.0)),
                "sim_gb_per_sec": float(dbm.get("sim_gb_per_sec", 0.0)),
                "logical_read_bytes": logical_read,
                "logical_write_bytes": logical_write,
                "media_read_bytes": media_read,
                "media_write_bytes": media_write,
                "read_ops": read_ops,
                "write_ops": write_ops,
                "simulated_read_delay_ns": read_delay,
                "simulated_write_delay_ns": write_delay,
                "simulated_read_fixed_delay_ns": read_fixed_delay,
                "simulated_write_fixed_delay_ns": write_fixed_delay,
                "simulated_read_queue_delay_ns": read_queue_delay,
                "simulated_read_media_delay_ns": read_media_delay,
                "simulated_read_arb_delay_ns": read_arb_delay,
                "simulated_write_queue_delay_ns": write_queue_delay,
                "simulated_write_media_delay_ns": write_media_delay,
                "read_prefetch_hits": read_prefetch_hits,
                "sim_read_fixed_latency_ns_per_op": sim_read_fixed_lat,
                "sim_write_fixed_latency_ns_per_op": sim_write_fixed_lat,
                "sim_read_latency_ns_per_op": sim_read_lat,
                "sim_write_latency_ns_per_op": sim_write_lat,
                "sim_read_queue_latency_ns_per_op": sim_read_queue_lat,
                "sim_read_media_latency_ns_per_op": sim_read_media_lat,
                "sim_read_arb_latency_ns_per_op": sim_read_arb_lat,
                "sim_write_queue_latency_ns_per_op": sim_write_queue_lat,
                "sim_write_media_latency_ns_per_op": sim_write_media_lat,
                "sim_read_bw_mib_s": sim_read_bw,
                "sim_write_bw_mib_s": sim_write_bw,
                "prefetch_hit_ratio": prefetch_hit_ratio,
                "ns_per_line_read": ns_per_line_read,
                "ns_per_line_write": ns_per_line_write,
                "ewr_paper": ewr_paper,
                "write_amplification": write_amp,
            }
            rows.append(row)

    by_case = {str(r["case_id"]): r for r in rows}
    checks: List[Dict[str, object]] = []

    # Fig.2 / write visible latency anchors
    append_anchor(
        checks,
        "fig2_read_seq",
        "sim_read_latency_ns_per_op",
        169.0,
        float(by_case.get("lat_read_seq_256_local", {}).get("sim_read_latency_ns_per_op", math.nan)),
        10.0,
    )
    append_anchor(
        checks,
        "fig2_read_rand",
        "sim_read_latency_ns_per_op",
        305.0,
        float(by_case.get("lat_read_rand_256_local", {}).get("sim_read_latency_ns_per_op", math.nan)),
        10.0,
    )
    append_anchor(
        checks,
        "write_visible_ntstore",
        "sim_write_latency_ns_per_op",
        90.0,
        float(by_case.get("lat_write_rand_64_nt_local", {}).get("sim_write_latency_ns_per_op", math.nan)),
        15.0,
    )
    append_anchor(
        checks,
        "write_visible_clwb",
        "sim_write_latency_ns_per_op",
        62.0,
        float(by_case.get("lat_write_rand_64_clwb_local", {}).get("sim_write_latency_ns_per_op", math.nan)),
        15.0,
    )

    # EWR anchors (paper definition)
    append_anchor(
        checks,
        "ewr_rand_64_ntstore",
        "ewr_paper",
        0.25,
        float(by_case.get("ewr_rand_v64_local", {}).get("ewr_paper", math.nan)),
        20.0,
    )
    append_anchor(
        checks,
        "ewr_rand_256_ntstore",
        "ewr_paper",
        0.98,
        float(by_case.get("ewr_rand_v256_local", {}).get("ewr_paper", math.nan)),
        10.0,
    )

    # Fig.16 proxy points (threads 1/4/8/16 -> 4-point vectors)
    fig16_targets = {
        "bw_read_seq": [30.0, 19.1, 32.8, 25.6],
        "bw_read_rand": [25.7, 22.2, 26.3, 25.5],
        "bw_write_seq": [8.6, 7.0, 9.2, 8.5],
        "bw_write_rand": [6.7, 6.0, 8.2, 7.5],
    }
    fig16_cases = {
        "bw_read_seq": [
            "bw_read_seq_t1_local",
            "bw_read_seq_t4_local",
            "bw_read_seq_t8_local",
            "bw_read_seq_t16_local",
        ],
        "bw_read_rand": [
            "bw_read_rand_t1_local",
            "bw_read_rand_t4_local",
            "bw_read_rand_t8_local",
            "bw_read_rand_t16_local",
        ],
        "bw_write_seq": [
            "bw_write_seq_t1_local",
            "bw_write_seq_t4_local",
            "bw_write_seq_t8_local",
            "bw_write_seq_t16_local",
        ],
        "bw_write_rand": [
            "bw_write_rand_t1_local",
            "bw_write_rand_t4_local",
            "bw_write_rand_t8_local",
            "bw_write_rand_t16_local",
        ],
    }
    for line_name, targets in fig16_targets.items():
        cases = fig16_cases[line_name]
        for idx, target in enumerate(targets):
            case_id = cases[idx]
            actual = float(by_case.get(case_id, {}).get("gb_per_sec", math.nan))
            append_anchor(
                checks,
                f"{line_name}_p{idx + 1}",
                "db_bw_gb_per_sec",
                target,
                actual,
                35.0,
            )

    # Fig.17 peak thread + remote ratio anchors
    read_local_peak_bw, read_local_peak_thread = find_peak(rows, "conc_read_local_t")
    read_remote_peak_bw, read_remote_peak_thread = find_peak(rows, "conc_read_remote_t")
    write_local_peak_bw, write_local_peak_thread = find_peak(rows, "conc_write_local_t")
    write_remote_peak_bw, _ = find_peak(rows, "conc_write_remote_t")

    append_anchor(checks, "fig17_read_local_peak_thread", "thread", 16.0,
                  float(read_local_peak_thread), 0.0)
    append_anchor(checks, "fig17_read_remote_peak_thread", "thread", 10.0,
                  float(read_remote_peak_thread), 0.0)
    append_anchor(checks, "fig17_write_peak_thread", "thread", 4.0,
                  float(write_local_peak_thread), 0.0)
    append_anchor(
        checks,
        "fig17_remote_read_ratio",
        "ratio",
        0.592,
        safe_div(read_remote_peak_bw, read_local_peak_bw),
        20.0,
    )
    append_anchor(
        checks,
        "fig17_remote_write_ratio",
        "ratio",
        0.617,
        safe_div(write_remote_peak_bw, write_local_peak_bw),
        20.0,
    )

    summary_fields = list(rows[0].keys()) if rows else []
    if summary_fields:
        with summary_csv.open("w", encoding="utf-8", newline="") as f:
            w = csv.DictWriter(f, fieldnames=summary_fields)
            w.writeheader()
            for r in rows:
                w.writerow(r)

    anchor_fields = [
        "anchor",
        "metric",
        "target",
        "actual",
        "error_pct",
        "tolerance_pct",
        "pass",
    ]
    with anchors_csv.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=anchor_fields)
        w.writeheader()
        for c in checks:
            w.writerow(c)

    passed = sum(1 for c in checks if bool(c.get("pass", False)))
    total = len(checks)
    lines: List[str] = []
    lines.append("# Env FS Profile Report")
    lines.append("")
    lines.append(f"- run_dir: `{run_dir}`")
    lines.append(f"- summary_csv: `{summary_csv}`")
    lines.append(f"- anchors_csv: `{anchors_csv}`")
    lines.append(f"- anchors_passed: {passed}/{total}")
    lines.append("")
    lines.append("## Key Curves")
    lines.append(
        f"- read local peak: thread={read_local_peak_thread}, MB/s={read_local_peak_bw:.3f}"
    )
    lines.append(
        f"- read remote peak: thread={read_remote_peak_thread}, MB/s={read_remote_peak_bw:.3f}"
    )
    lines.append(
        f"- write local peak: thread={write_local_peak_thread}, MB/s={write_local_peak_bw:.3f}"
    )
    lines.append(
        f"- remote/local peak read ratio: {safe_div(read_remote_peak_bw, read_local_peak_bw):.4f}"
    )
    lines.append(
        f"- remote/local peak write ratio: {safe_div(write_remote_peak_bw, write_local_peak_bw):.4f}"
    )
    lines.append("")
    lines.append("## Anchor Checks")
    lines.append("| anchor | metric | target | actual | error_pct | tolerance_pct | pass |")
    lines.append("|---|---|---:|---:|---:|---:|---|")
    for c in checks:
        target = float(c["target"])
        actual = float(c["actual"]) if not math.isnan(float(c["actual"])) else math.nan
        err = float(c["error_pct"]) if not math.isnan(float(c["error_pct"])) else math.nan
        target_s = f"{target:.6f}"
        actual_s = "NA" if math.isnan(actual) else f"{actual:.6f}"
        err_s = "NA" if math.isnan(err) else f"{err:.4f}"
        lines.append(
            f"| {c['anchor']} | {c['metric']} | {target_s} | {actual_s} | {err_s} | {float(c['tolerance_pct']):.4f} | {str(bool(c['pass'])).lower()} |"
        )

    report_md.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"summary: {summary_csv}")
    print(f"anchors: {anchors_csv}")
    print(f"report: {report_md}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
