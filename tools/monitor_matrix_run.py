#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Optional


@dataclass
class BenchState:
    pid: int
    cmd: str
    bench: str
    num: int
    reads: int
    threads: int
    progress: int
    target: int
    pct: float


def now_str() -> str:
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def run_cmd(args: list[str]) -> str:
    try:
        out = subprocess.check_output(args, stderr=subprocess.DEVNULL, text=True)
        return out.strip()
    except Exception:
        return ""


def list_procs(pattern: str) -> list[tuple[int, str]]:
    out = run_cmd(["pgrep", "-fa", pattern])
    if not out:
        return []
    res: list[tuple[int, str]] = []
    for line in out.splitlines():
        parts = line.split(" ", 1)
        if len(parts) != 2:
            continue
        try:
            pid = int(parts[0])
        except ValueError:
            continue
        cmd = parts[1]
        if "monitor_matrix_run.py" in cmd:
            continue
        res.append((pid, cmd))
    return res


def parse_flag(cmd: str, key: str) -> str:
    m = re.search(rf"{re.escape(key)}=([^ ]+)", cmd)
    return m.group(1) if m else ""


def read_tail(path: Path, max_bytes: int = 4_000_000) -> str:
    if not path.exists():
        return ""
    size = path.stat().st_size
    start = max(0, size - max_bytes)
    with path.open("rb") as f:
        f.seek(start)
        return f.read().decode("utf-8", errors="ignore")


def count_total_runs(plan_md: Path) -> int:
    if not plan_md.exists():
        return 0
    pat = re.compile(r"^\|\s+\d+\s+\|")
    return sum(1 for ln in plan_md.read_text(encoding="utf-8", errors="ignore").splitlines() if pat.match(ln))


def count_done_runs(registry_csv: Path) -> tuple[int, int]:
    if not registry_csv.exists():
        return (0, 0)
    done = 0
    skipped = 0
    with registry_csv.open("r", encoding="utf-8", errors="ignore", newline="") as f:
        for row in csv.DictReader(f):
            st = (row.get("status") or "").strip().lower()
            if st == "done":
                done += 1
            elif st == "skipped":
                skipped += 1
    return (done, skipped)


def parse_bench_state(runner_log: Path, db_proc: Optional[tuple[int, str]]) -> Optional[BenchState]:
    if db_proc is None:
        return None
    pid, cmd = db_proc
    bench = parse_flag(cmd, "--benchmarks")
    num = int(parse_flag(cmd, "--num") or 0)
    reads = int(parse_flag(cmd, "--reads") or 0)
    threads = int(parse_flag(cmd, "--threads") or 0)

    text = read_tail(runner_log)
    matches = [int(x) for x in re.findall(r"finished\s+([0-9]+)\s+ops", text)]
    max_ops = max(matches) if matches else 0
    last_ops = matches[-1] if matches else 0

    progress = last_ops
    target = 0
    if bench == "fillrandom,stats":
        target = num
        if max_ops > 0 and threads > 0:
            progress = min(max_ops * threads, target)
    elif bench in ("mixgraph,stats", "seekrandom,stats"):
        if reads > 0 and threads > 0:
            target = reads * threads
            if max_ops > 0:
                progress = min(max_ops * threads, target)
        elif reads > 0:
            target = reads
            if max_ops > 0:
                progress = min(max_ops, target)

    pct = (progress * 100.0 / target) if target > 0 else 0.0
    return BenchState(
        pid=pid,
        cmd=cmd,
        bench=bench,
        num=num,
        reads=reads,
        threads=threads,
        progress=progress,
        target=target,
        pct=pct,
    )


def disk_free_gb(path: Path) -> float:
    try:
        free = shutil.disk_usage(path).free
    except Exception:
        return -1.0
    return free / (1024 ** 3)


def append_event(path: Path, level: str, msg: str) -> None:
    line = f"[{now_str()}] {level}: {msg}\n"
    with path.open("a", encoding="utf-8") as f:
        f.write(line)


def write_status(path: Path, payload: dict) -> None:
    path.write_text(json.dumps(payload, ensure_ascii=True, indent=2), encoding="utf-8")


def stop_processes(pattern: str) -> None:
    pids = [pid for pid, _ in list_procs(pattern)]
    for pid in pids:
        try:
            os.kill(pid, signal.SIGTERM)
        except Exception:
            pass
    time.sleep(1.0)
    for pid in pids:
        try:
            os.kill(pid, 0)
            os.kill(pid, signal.SIGKILL)
        except Exception:
            pass


def main() -> int:
    ap = argparse.ArgumentParser(description="Monitor matrix benchmark run and detect faults.")
    ap.add_argument("--matrix-dir", required=True, help="Matrix directory (e.g. experiment/20260205_standard_matrix_50gb_exp1)")
    ap.add_argument("--refresh", type=float, default=20.0, help="Polling interval seconds")
    ap.add_argument("--stall-seconds", type=int, default=900, help="Warn when progress does not move for N seconds")
    ap.add_argument("--min-free-gb", type=float, default=5.0, help="Minimum free disk GB before alerting")
    ap.add_argument("--stop-on-fatal", type=int, default=1, help="1 to stop jobs on fatal issues")
    args = ap.parse_args()

    matrix_dir = Path(args.matrix_dir).resolve()
    runner_log = matrix_dir / "matrix_runner.log"
    plan_md = matrix_dir / "matrix_plan.md"
    registry_csv = matrix_dir / "run_registry.csv"
    events_log = matrix_dir / "monitor_events.log"
    status_json = matrix_dir / "monitor_status.json"
    summary_txt = matrix_dir / "monitor_summary.txt"

    if not matrix_dir.exists():
        print(f"matrix dir not found: {matrix_dir}", file=sys.stderr)
        return 1
    if not runner_log.exists():
        print(f"runner log not found: {runner_log}", file=sys.stderr)
        return 1

    append_event(events_log, "INFO", "monitor started")

    seen_error_lines: set[str] = set()
    last_progress = -1
    last_progress_ts = time.time()
    fatal_reason = ""

    error_pat = re.compile(
        r"(No space left on device|IO error|Corruption|assert|Assertion|FATAL|Segmentation fault|Aborted)",
        re.IGNORECASE,
    )

    while True:
        ts = time.time()
        total_runs = count_total_runs(plan_md)
        done_runs, skipped_runs = count_done_runs(registry_csv)

        matrix_procs = list_procs(r"run_standard_matrix_50gb.sh")
        db_procs = list_procs(r"db_bench")
        db_state = parse_bench_state(runner_log, db_procs[0] if db_procs else None)

        # Fault check: log errors in runner output.
        tail_text = read_tail(runner_log)
        for ln in tail_text.splitlines():
            if error_pat.search(ln):
                if ln not in seen_error_lines:
                    seen_error_lines.add(ln)
                    append_event(events_log, "ERROR", f"runner_log_match: {ln[:400]}")

        # Disk free check
        paths_to_check = [matrix_dir, Path("/tmp")]
        if db_state:
            db_path = parse_flag(db_state.cmd, "--db")
            if db_path:
                paths_to_check.append(Path(db_path))
        min_free_observed = 10_000.0
        for p in paths_to_check:
            free_gb = disk_free_gb(p)
            if free_gb >= 0:
                min_free_observed = min(min_free_observed, free_gb)
        if min_free_observed < args.min_free_gb:
            fatal_reason = f"low disk free: {min_free_observed:.2f} GB < {args.min_free_gb:.2f} GB"

        # Stall check
        if db_state and db_state.progress > 0:
            if db_state.progress != last_progress:
                last_progress = db_state.progress
                last_progress_ts = ts
            elif ts - last_progress_ts >= args.stall_seconds:
                append_event(events_log, "WARN", f"progress stalled for {(ts - last_progress_ts):.0f}s at {db_state.progress}/{db_state.target}")
                last_progress_ts = ts  # avoid duplicate spam

        # Completion check
        no_active = (len(matrix_procs) == 0 and len(db_procs) == 0)
        finished = no_active and total_runs > 0 and (done_runs + skipped_runs) >= total_runs

        status = {
            "time": now_str(),
            "matrix_dir": str(matrix_dir),
            "total_runs": total_runs,
            "done_runs": done_runs,
            "skipped_runs": skipped_runs,
            "matrix_proc_count": len(matrix_procs),
            "db_proc_count": len(db_procs),
            "min_free_gb": round(min_free_observed, 3) if min_free_observed < 9_999 else None,
            "fatal_reason": fatal_reason or None,
            "finished": finished,
        }
        if db_state:
            status["current"] = {
                "pid": db_state.pid,
                "bench": db_state.bench,
                "progress": db_state.progress,
                "target": db_state.target,
                "pct": round(db_state.pct, 3) if db_state.target > 0 else None,
            }
        write_status(status_json, status)

        if fatal_reason:
            append_event(events_log, "FATAL", fatal_reason)
            if args.stop_on_fatal == 1:
                stop_processes(r"run_standard_matrix_50gb.sh|run_release_50gb_quick.sh|db_bench")
                append_event(events_log, "FATAL", "sent SIGTERM to benchmark processes")
            summary_txt.write_text(
                f"[{now_str()}] monitor stopped by fatal condition\nreason: {fatal_reason}\n",
                encoding="utf-8",
            )
            return 2

        if finished:
            append_event(events_log, "INFO", "matrix finished")
            summary_txt.write_text(
                f"[{now_str()}] monitor finished\nruns: done={done_runs}, skipped={skipped_runs}, total={total_runs}\n",
                encoding="utf-8",
            )
            return 0

        time.sleep(max(args.refresh, 2.0))


if __name__ == "__main__":
    raise SystemExit(main())
