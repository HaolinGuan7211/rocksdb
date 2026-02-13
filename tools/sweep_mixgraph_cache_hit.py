#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import math
import re
import shlex
import subprocess
from pathlib import Path
from typing import Dict, List, Optional

import matplotlib.pyplot as plt


MIXGRAPH_RE = re.compile(
    r"^mixgraph\s+:\s+([0-9.]+)\s+micros/op\s+([0-9]+)\s+ops/sec"
)
SEEK_RE = re.compile(
    r"^rocksdb\.db\.seek\.micros P50 : ([0-9.]+) P95 : ([0-9.]+) P99 : ([0-9.]+)"
)
HIT_RE = re.compile(r"^rocksdb\.block\.cache\.hit COUNT : ([0-9.]+)")
MISS_RE = re.compile(r"^rocksdb\.block\.cache\.miss COUNT : ([0-9.]+)")


def parse_list_mb(raw: str) -> List[float]:
    vals: List[float] = []
    for item in raw.split(","):
        t = item.strip()
        if not t:
            continue
        vals.append(float(t))
    if not vals:
        raise ValueError("empty cache size list")
    return vals


def run_cmd(cmd: List[str], log_path: Path) -> str:
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False)
    log_path.write_text(proc.stdout, encoding="utf-8")
    if proc.returncode != 0:
        raise RuntimeError(f"db_bench failed ({proc.returncode}) for {log_path.name}")
    return proc.stdout


def parse_output(text: str) -> Dict[str, float]:
    out: Dict[str, float] = {
        "ops_per_sec": float("nan"),
        "micros_per_op": float("nan"),
        "seek_p95_us": float("nan"),
        "seek_p99_us": float("nan"),
        "cache_hit": 0.0,
        "cache_miss": 0.0,
        "cache_hit_ratio_pct": float("nan"),
    }
    for raw in text.splitlines():
        line = raw.strip()
        m = MIXGRAPH_RE.match(line)
        if m:
            out["micros_per_op"] = float(m.group(1))
            out["ops_per_sec"] = float(m.group(2))
            continue
        m = SEEK_RE.match(line)
        if m:
            out["seek_p95_us"] = float(m.group(2))
            out["seek_p99_us"] = float(m.group(3))
            continue
        m = HIT_RE.match(line)
        if m:
            out["cache_hit"] = float(m.group(1))
            continue
        m = MISS_RE.match(line)
        if m:
            out["cache_miss"] = float(m.group(1))
            continue
    total = out["cache_hit"] + out["cache_miss"]
    if total > 0:
        out["cache_hit_ratio_pct"] = out["cache_hit"] * 100.0 / total
    return out


def write_csv(rows: List[Dict[str, float]], path: Path) -> None:
    if not rows:
        return
    keys = list(rows[0].keys())
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        for r in rows:
            w.writerow(r)


def plot(rows: List[Dict[str, float]], target_hit: float, out_dir: Path) -> None:
    x = [r["cache_mb"] for r in rows]
    y_hit = [r["cache_hit_ratio_pct"] for r in rows]
    y_ops = [r["ops_per_sec"] for r in rows]
    y_p99 = [r["seek_p99_us"] for r in rows]

    fig, ax = plt.subplots(figsize=(9.5, 5.2))
    ax.semilogx(x, y_hit, marker="o", linewidth=2.0, color="#1f77b4")
    ax.axhline(target_hit, color="#d62728", linestyle="--", linewidth=1.5, label=f"target {target_hit:.1f}%")
    ax.set_xlabel("Block cache size (MiB, log scale)")
    ax.set_ylabel("Cache hit ratio (%)")
    ax.set_title("Mixgraph Hit Ratio vs Block Cache Size")
    ax.grid(alpha=0.25, which="both")
    ax.legend()
    plt.tight_layout()
    plt.savefig(out_dir / "hit_ratio_vs_cache.png", dpi=180)
    plt.close(fig)

    fig, ax1 = plt.subplots(figsize=(9.5, 5.2))
    ax1.semilogx(x, y_ops, marker="o", linewidth=2.0, color="#1f77b4")
    ax1.set_xlabel("Block cache size (MiB, log scale)")
    ax1.set_ylabel("Throughput (ops/sec)", color="#1f77b4")
    ax1.tick_params(axis="y", labelcolor="#1f77b4")
    ax1.grid(alpha=0.25, which="both")
    ax2 = ax1.twinx()
    ax2.semilogx(x, y_p99, marker="s", linewidth=2.0, color="#d62728")
    ax2.set_ylabel("Seek p99 (us)", color="#d62728")
    ax2.tick_params(axis="y", labelcolor="#d62728")
    plt.title("Mixgraph Throughput / P99 vs Block Cache Size")
    plt.tight_layout()
    plt.savefig(out_dir / "ops_p99_vs_cache.png", dpi=180)
    plt.close(fig)


def nearest_to_target(rows: List[Dict[str, float]], target: float) -> Optional[Dict[str, float]]:
    valid = [r for r in rows if not math.isnan(r["cache_hit_ratio_pct"])]
    if not valid:
        return None
    return min(valid, key=lambda r: abs(r["cache_hit_ratio_pct"] - target))


def write_report(rows: List[Dict[str, float]], target_hit: float, out_md: Path) -> None:
    near = nearest_to_target(rows, target_hit)
    lines: List[str] = []
    lines.append("# Experiment Result - Cache Hit Ratio Target Search")
    lines.append("")
    lines.append(f"- target_hit_ratio: `{target_hit:.1f}%`")
    lines.append(f"- tested_cache_sizes_mib: `{', '.join(str(int(r['cache_mb']) if r['cache_mb'].is_integer() else r['cache_mb']) for r in rows)}`")
    if near is not None:
        lines.append("")
        lines.append("## Nearest to target")
        lines.append(
            f"- cache: `{near['cache_mb']:.3f} MiB`, hit_ratio: `{near['cache_hit_ratio_pct']:.4f}%`, "
            f"ops: `{near['ops_per_sec']:.0f}`, seek_p99: `{near['seek_p99_us']:.3f} us`"
        )
    lines.append("")
    lines.append("## Artifacts")
    lines.append("- `results.csv`")
    lines.append("- `hit_ratio_vs_cache.png`")
    lines.append("- `ops_p99_vs_cache.png`")
    out_md.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description="Sweep block cache sizes for mixgraph hit-ratio target.")
    ap.add_argument("--db-bench", default="build/db_bench")
    ap.add_argument("--db-path", required=True)
    ap.add_argument("--wal-root", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--num", type=int, default=197379011)
    ap.add_argument("--reads", type=int, default=750000)
    ap.add_argument("--threads", type=int, default=16)
    ap.add_argument("--key-size", type=int, default=16)
    ap.add_argument("--compression", default="none")
    ap.add_argument("--target-hit-ratio", type=float, default=90.0)
    ap.add_argument(
        "--cache-sizes-mib",
        default="1024,512,256,128,64,32,16,8,4,2,1,0.5,0.25,0.125",
        help="comma-separated MiB values",
    )
    args = ap.parse_args()

    db_bench = Path(args.db_bench).resolve()
    db_path = Path(args.db_path).resolve()
    wal_root = Path(args.wal_root).resolve()
    out_dir = Path(args.out_dir).resolve()
    logs_dir = out_dir / "logs"
    out_dir.mkdir(parents=True, exist_ok=True)
    logs_dir.mkdir(parents=True, exist_ok=True)
    wal_root.mkdir(parents=True, exist_ok=True)

    cache_sizes_mib = parse_list_mb(args.cache_sizes_mib)
    cache_sizes_mib = sorted(cache_sizes_mib, reverse=True)

    rows: List[Dict[str, float]] = []
    for mib in cache_sizes_mib:
        cache_bytes = max(1024, int(mib * 1024 * 1024))
        wal_dir = wal_root / f"wal_{str(mib).replace('.', '_')}mib"
        wal_dir.mkdir(parents=True, exist_ok=True)
        log_path = logs_dir / f"mixgraph_cache_{str(mib).replace('.', '_')}mib.log"

        cmd = [
            str(db_bench),
            f"--db={db_path}",
            f"--wal_dir={wal_dir}",
            "--use_existing_db=1",
            "--benchmarks=mixgraph,stats",
            "--statistics",
            f"--num={args.num}",
            f"--reads={args.reads}",
            f"--threads={args.threads}",
            f"--key_size={args.key_size}",
            f"--compression_type={args.compression}",
            f"--cache_size={cache_bytes}",
            "--use_direct_reads=true",
            "--use_direct_io_for_flush_and_compaction=true",
            "--value_k=0.9",
            "--value_sigma=256",
            "--value_theta=0",
            "--key_dist_a=0.0016",
            "--key_dist_b=-0.71",
            "--keyrange_dist_a=14.18",
            "--keyrange_dist_b=-2.917",
            "--keyrange_dist_c=0.0164",
            "--keyrange_dist_d=-0.08082",
            "--keyrange_num=32",
            "--iter_k=0.08",
            "--iter_sigma=1.75",
            "--iter_theta=0",
            "--mix_get_ratio=0.20",
            "--mix_put_ratio=0.00",
            "--mix_seek_ratio=0.80",
            "--seed=1",
        ]
        print("RUN:", " ".join(shlex.quote(x) for x in cmd))
        output = run_cmd(cmd, log_path)
        parsed = parse_output(output)
        parsed["cache_mb"] = float(mib)
        parsed["cache_bytes"] = float(cache_bytes)
        rows.append(parsed)
        print(
            f"cache={mib}MiB hit={parsed['cache_hit_ratio_pct']:.4f}% "
            f"ops={parsed['ops_per_sec']:.0f} p99={parsed['seek_p99_us']:.3f}"
        )

    write_csv(rows, out_dir / "results.csv")
    plot(rows, args.target_hit_ratio, out_dir)
    write_report(rows, args.target_hit_ratio, out_dir / "report.md")
    print(f"done: {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
