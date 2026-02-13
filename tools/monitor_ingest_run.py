#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import shutil
import time
from datetime import datetime
from pathlib import Path


ERROR_PATTERNS = (
    "No space left on device",
    "IO error",
    "Corruption",
    "FATAL",
    "Segmentation fault",
    "Aborted",
)


def now() -> str:
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def read_tail(path: Path, max_bytes: int = 2_000_000) -> str:
    if not path.exists():
        return ""
    size = path.stat().st_size
    start = max(0, size - max_bytes)
    with path.open("rb") as f:
        f.seek(start)
        return f.read().decode("utf-8", errors="ignore")


def disk_free_gb(path: Path) -> float:
    return shutil.disk_usage(path).free / (1024**3)


def append(path: Path, line: str) -> None:
    with path.open("a", encoding="utf-8") as f:
        f.write(line + "\n")


def main() -> int:
    ap = argparse.ArgumentParser(description="Monitor SST ingest run for disk/errors.")
    ap.add_argument("--experiment-dir", required=True)
    ap.add_argument("--db-root", required=True)
    ap.add_argument("--refresh", type=float, default=20.0)
    ap.add_argument("--min-free-gb", type=float, default=5.0)
    args = ap.parse_args()

    exp_dir = Path(args.experiment_dir).resolve()
    runner = exp_dir / "run_results"
    events = exp_dir / "ingest_monitor_events.log"
    status = exp_dir / "ingest_monitor_status.json"
    summary = exp_dir / "ingest_monitor_summary.txt"

    append(events, f"[{now()}] INFO: monitor started")
    seen = set()

    while True:
        # track latest run dir
        run_dirs = sorted((p for p in runner.glob("*") if p.is_dir()), key=lambda p: p.stat().st_mtime)
        run_dir = run_dirs[-1] if run_dirs else None
        runner_log = run_dir / "runner.log" if run_dir else None
        tail = read_tail(runner_log) if runner_log else ""

        for line in tail.splitlines():
            for pat in ERROR_PATTERNS:
                if pat.lower() in line.lower():
                    key = (pat, line)
                    if key not in seen:
                        seen.add(key)
                        append(events, f"[{now()}] ERROR: {line[:500]}")

        free_root = disk_free_gb(Path("/"))
        free_db = disk_free_gb(Path(args.db_root))
        min_free = min(free_root, free_db)
        if min_free < args.min_free_gb:
            append(events, f"[{now()}] ERROR: low disk free {min_free:.2f}GiB < {args.min_free_gb:.2f}GiB")

        finished = False
        if run_dir and runner_log and runner_log.exists():
            txt = read_tail(runner_log, 200_000)
            if "done, run_dir=" in txt:
                finished = True

        payload = {
            "time": now(),
            "experiment_dir": str(exp_dir),
            "active_run_dir": str(run_dir) if run_dir else "",
            "free_gb_root": round(free_root, 3),
            "free_gb_db_root": round(free_db, 3),
            "finished": finished,
        }
        status.write_text(json.dumps(payload, ensure_ascii=True, indent=2), encoding="utf-8")
        summary.write_text(
            "\n".join(
                [
                    f"time: {payload['time']}",
                    f"experiment_dir: {payload['experiment_dir']}",
                    f"active_run_dir: {payload['active_run_dir'] or 'NA'}",
                    f"free_gb_root: {payload['free_gb_root']}",
                    f"free_gb_db_root: {payload['free_gb_db_root']}",
                    f"finished: {payload['finished']}",
                    f"events_log: {events}",
                ]
            )
            + "\n",
            encoding="utf-8",
        )

        if finished:
            append(events, f"[{now()}] INFO: monitor finished")
            return 0

        time.sleep(max(1.0, args.refresh))


if __name__ == "__main__":
    raise SystemExit(main())
