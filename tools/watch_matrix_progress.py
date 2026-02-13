#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import re
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Optional


def run_cmd(args: list[str]) -> str:
    try:
        out = subprocess.check_output(args, stderr=subprocess.DEVNULL, text=True)
    except Exception:
        return ""
    return out.strip()


def format_duration(seconds: int) -> str:
    if seconds < 0:
        return "NA"
    h = seconds // 3600
    m = (seconds % 3600) // 60
    s = seconds % 60
    return f"{h:02d}:{m:02d}:{s:02d}"


def colorize(text: str, code: str, enabled: bool) -> str:
    if not enabled:
        return text
    return f"\033[{code}m{text}\033[0m"


def progress_bar(ratio: float, width: int, color: bool) -> str:
    ratio = max(0.0, min(1.0, ratio))
    filled = int(round(ratio * width))
    if ratio > 0 and filled == 0:
        filled = 1
    if color:
        full = colorize("■", "38;5;39", True)
        empty = colorize("□", "38;5;240", True)
    else:
        full = "■"
        empty = "□"
    return (full * filled) + (empty * (width - filled))


def latest_matrix_dir(root: Path) -> Optional[Path]:
    candidates = sorted(
        root.glob("experiment/*_standard_matrix_50gb_exp*"),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    return candidates[0] if candidates else None


def read_tail_text(path: Path, max_bytes: int = 2_000_000) -> str:
    if not path.exists():
        return ""
    size = path.stat().st_size
    start = max(0, size - max_bytes)
    with path.open("rb") as f:
        f.seek(start)
        return f.read().decode("utf-8", errors="ignore")


def has_base_stage(runner_log: Path) -> bool:
    text = read_tail_text(runner_log)
    return ("[matrix] prepare base DB once" in text) or ("[matrix] reuse prepared base DB" in text)


def count_total_runs(plan_md: Path) -> int:
    if not plan_md.exists():
        return 0
    total = 0
    pattern = re.compile(r"^\|\s+\d+\s+\|")
    for line in plan_md.read_text(encoding="utf-8", errors="ignore").splitlines():
        if pattern.match(line):
            total += 1
    return total


def count_status_runs(registry_csv: Path) -> tuple[int, int]:
    if not registry_csv.exists():
        return (0, 0)
    done = 0
    skipped = 0
    with registry_csv.open("r", encoding="utf-8", errors="ignore", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            status = (row.get("status") or "").strip().lower()
            if status == "done":
                done += 1
            elif status == "skipped":
                skipped += 1
    return (done, skipped)


def last_finished_ops(runner_log: Path) -> int:
    text = read_tail_text(runner_log)
    matches = re.findall(r"finished\s+([0-9]+)\s+ops", text)
    return int(matches[-1]) if matches else 0


def max_finished_ops(runner_log: Path) -> int:
    text = read_tail_text(runner_log)
    matches = re.findall(r"finished\s+([0-9]+)\s+ops", text)
    if not matches:
        return 0
    return max(int(x) for x in matches)


def last_matrix_start(runner_log: Path) -> str:
    text = read_tail_text(runner_log)
    lines = re.findall(r"^\[matrix\].*start seq=.*$", text, flags=re.MULTILINE)
    return lines[-1] if lines else ""


def parse_field(text: str, key: str) -> str:
    m = re.search(rf"{re.escape(key)}=([^ ]+)", text)
    return m.group(1) if m else ""


def parse_config_kv(path: Path) -> dict[str, str]:
    res: dict[str, str] = {}
    if not path.exists():
        return res
    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        res[k.strip()] = v.strip()
    return res


def to_int(value: str, default: int = 0) -> int:
    return int(value) if value.isdigit() else default


def case_slug_from_experiment_name(exp_name: str) -> str:
    m = re.match(r"^experiment\d+_(.+)$", exp_name)
    return m.group(1) if m else ""


def latest_run_dir(case_dir: Path) -> Optional[Path]:
    run_root = case_dir / "run_results"
    if not run_root.exists():
        return None
    runs = sorted([p for p in run_root.iterdir() if p.is_dir()], key=lambda p: p.stat().st_mtime, reverse=True)
    return runs[0] if runs else None


def latest_case_run(matrix_dir: Path) -> tuple[Optional[Path], Optional[Path]]:
    cases_dir = matrix_dir / "cases"
    if not cases_dir.exists():
        return (None, None)
    latest: Optional[Path] = None
    latest_case: Optional[Path] = None
    for case_dir in [p for p in cases_dir.iterdir() if p.is_dir()]:
        run_dir = latest_run_dir(case_dir)
        if not run_dir:
            continue
        if latest is None or run_dir.stat().st_mtime > latest.stat().st_mtime:
            latest = run_dir
            latest_case = case_dir
    return (latest_case, latest)


def calc_progress(
    bench: str,
    num: str,
    reads: str,
    threads: str,
    progress_raw_last: int,
    progress_raw_max: int,
) -> tuple[int, str, int, str]:
    target = 0
    target_note = ""
    progress = 0
    progress_source = "unknown"
    if bench == "fillrandom,stats":
        target = int(num) if num.isdigit() else 0
        if target > 0 and threads.isdigit() and progress_raw_max > 0:
            progress = min(progress_raw_max * int(threads), target)
            progress_source = "max_finished_ops*threads"
    elif bench in ("mixgraph,stats", "seekrandom,stats"):
        if reads.isdigit() and threads.isdigit():
            target = int(reads) * int(threads)
            target_note = "(reads*threads)"
            if progress_raw_max > 0:
                progress = min(progress_raw_max * int(threads), target)
                progress_source = "max_finished_ops*threads"
        elif reads.isdigit():
            target = int(reads)
            if progress_raw_max > 0:
                progress = min(progress_raw_max, target)
                progress_source = "max_finished_ops"
    if progress == 0:
        progress = progress_raw_last
    return target, target_note, progress, progress_source


def step_id_from_name(step_name: str) -> str:
    m = re.match(r"^([0-9]{2})_", step_name)
    return m.group(1) if m else ""


def case_read_targets_from_config(config: dict[str, str]) -> dict[str, int]:
    threads = to_int(config.get("THREADS", "0"))
    mix = to_int(config.get("REALISTIC_READS", "0")) * threads
    s200 = to_int(config.get("STEP200_READS", "0")) * threads
    w1 = to_int(config.get("WORST_READS_1", "0")) * threads
    w4 = to_int(config.get("WORST_READS_4", "0")) * threads
    w20 = to_int(config.get("WORST_READS_20", "0")) * threads
    w200 = to_int(config.get("WORST_READS_200", "0")) * threads
    # step 08 is hard-coded to threads=8 in run_shortscan_compare.sh
    w10000 = to_int(config.get("WORST_READS_10000", "0")) * 8
    return {
        "02": mix,
        "03": s200,
        "04": w1,
        "05": w4,
        "06": w20,
        "07": w200,
        "08": w10000,
    }


def parse_case_progress(
    case_runner_log: Path,
    targets: dict[str, int],
    current_step_id: str,
    current_step_progress: int,
) -> tuple[int, int]:
    if not case_runner_log.exists():
        return (0, sum(targets.values()))
    text = case_runner_log.read_text(encoding="utf-8", errors="ignore")
    done_ids = set(re.findall(r"^\[[^\]]+\]\s+END\s+([0-9]{2})_[^\s]+", text, flags=re.MULTILINE))
    done = sum(targets.get(step_id, 0) for step_id in done_ids)
    total = sum(targets.values())
    if current_step_id in targets and current_step_id not in done_ids:
        done += min(max(current_step_progress, 0), targets[current_step_id])
    return (done, total)


def first_db_bench_line() -> str:
    out = run_cmd(["pgrep", "-fa", "db_bench"])
    if not out:
        return ""
    for line in out.splitlines():
        if "watch_matrix_progress.py" in line:
            continue
        return line
    return ""


def ps_elapsed_seconds(pid: str) -> int:
    if not pid:
        return 0
    out = run_cmd(["ps", "-p", pid, "-o", "etimes="])
    out = out.strip()
    return int(out) if out.isdigit() else 0


def has_active_process() -> bool:
    out = run_cmd(["pgrep", "-fa", "run_standard_matrix_50gb.sh|run_release_50gb_quick.sh|db_bench"])
    if not out:
        return False
    lines = [x for x in out.splitlines() if "watch_matrix_progress.py" not in x]
    return len(lines) > 0


def render_once(matrix_dir: Path, width: int, frame_idx: int) -> None:
    matrix_runner = matrix_dir / "matrix_runner.log"
    plan_md = matrix_dir / "matrix_plan.md"
    registry_csv = matrix_dir / "run_registry.csv"

    total = count_total_runs(plan_md)
    done, skipped = count_status_runs(registry_csv)

    latest_case, latest_run = latest_case_run(matrix_dir)
    runner_log = matrix_runner if matrix_runner.exists() else (latest_run / "runner.log" if latest_run else matrix_runner)

    start_line = last_matrix_start(matrix_runner) if matrix_runner.exists() else ""
    seq = parse_field(start_line, "seq")
    exp_id = parse_field(start_line, "exp_id")
    exp_name = parse_field(start_line, "name")
    if not exp_name and latest_run:
        cfg = parse_config_kv(latest_run / "config.txt")
        exp_name = cfg.get("EXPERIMENT_NAME", "")
        exp_id = cfg.get("GLOBAL_EXP_ID", "") or exp_id

    db_line = first_db_bench_line()
    db_pid = db_line.split(" ", 1)[0] if db_line else ""
    bench = parse_field(db_line, "--benchmarks")
    num = parse_field(db_line, "--num")
    reads = parse_field(db_line, "--reads")
    threads = parse_field(db_line, "--threads")
    seek_nexts = parse_field(db_line, "--seek_nexts")

    progress_raw_last = last_finished_ops(runner_log)
    progress_raw_max = max_finished_ops(runner_log)
    target, target_note, progress, progress_source = calc_progress(
        bench, num, reads, threads, progress_raw_last, progress_raw_max
    )
    if progress_raw_max == 0 and progress_raw_last == 0 and not runner_log.exists():
        progress_source = "runner_log_missing"

    is_tty = sys.stdout.isatty()
    if is_tty:
        print("\033[2J\033[H", end="")

    # Overall progresses only when a case is fully finished.
    overall_ratio = 0.0
    if total > 0:
        overall_ratio = max(0.0, min(1.0, (done + skipped) / total))
    overall_pct = overall_ratio * 100.0

    case_ratio = 0.0
    case_done_keys = 0
    case_total_keys = 0
    current_step_id = ""
    current_step_name = ""
    case_dir: Optional[Path] = None
    run_dir: Optional[Path] = None
    if exp_name:
        case_slug = case_slug_from_experiment_name(exp_name)
        if case_slug:
            case_dir = matrix_dir / "cases" / case_slug
            run_dir = latest_run_dir(case_dir)
    if not run_dir and latest_case and latest_run:
        case_dir = latest_case
        run_dir = latest_run
    if run_dir:
        cfg = parse_config_kv(run_dir / "config.txt")
        targets = case_read_targets_from_config(cfg)
        step_name_matches = re.findall(
            r"^\[[^\]]+\]\s+START\s+([0-9]{2}_[^\s]+)$",
            read_tail_text(run_dir / "runner.log", max_bytes=500_000),
            flags=re.MULTILINE,
        )
        current_step_name = step_name_matches[-1] if step_name_matches else ""
        current_step_id = step_id_from_name(current_step_name)
        case_done_keys, case_total_keys = parse_case_progress(
            run_dir / "runner.log", targets, current_step_id, progress
        )
        if case_total_keys > 0:
            case_ratio = max(0.0, min(1.0, case_done_keys / case_total_keys))
    if progress == 0 and run_dir and current_step_name:
        step_log = run_dir / f"{current_step_name}.log"
        if step_log.exists():
            progress_raw_last = last_finished_ops(step_log)
            progress_raw_max = max_finished_ops(step_log)
            target, target_note, progress, progress_source = calc_progress(
                bench, num, reads, threads, progress_raw_last, progress_raw_max
            )
            if progress_raw_max > 0:
                progress_source = f"step_log:{current_step_name}"

    elapsed = ps_elapsed_seconds(db_pid)
    current_ratio = 0.0
    pct = "NA"
    eta = "NA"
    if target > 0 and progress > 0:
        current_ratio = min(progress / target, 1.0)
        pct = f"{(current_ratio * 100.0):.2f}"
        eta_sec = int(elapsed * (target - progress) / progress)
        eta = format_duration(eta_sec)

    spinner = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"][frame_idx % 10]

    print(f"{spinner} Matrix Progress Watch (Python)")
    print(f"time: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"matrix_dir: {matrix_dir}")
    print()
    print(f"run_status: done={done} skipped={skipped} total={total}")
    print(f"overall(cases): [{progress_bar(overall_ratio, width, is_tty)}] {overall_pct:.2f}% ({done + skipped}/{total})")
    if seq:
        print(f"current_case: seq={seq} exp_id={exp_id or 'NA'} name={exp_name or 'NA'}")
    elif exp_name:
        print(f"current_case: exp_id={exp_id or 'NA'} name={exp_name or 'NA'}")
    else:
        print("current_case: not started yet")
    if case_total_keys > 0:
        print(
            f"case(read_keys): [{progress_bar(case_ratio, width, is_tty)}] "
            f"{case_ratio * 100.0:.2f}% ({case_done_keys}/{case_total_keys})"
        )
        if current_step_id:
            print(f"case_step_id: {current_step_id}")
    print()

    if db_line:
        print(f"db_bench_pid: {db_pid}")
        print(f"benchmarks: {bench or 'NA'}")
        if seek_nexts:
            print(f"seek_nexts: {seek_nexts}")
        if target > 0:
            print(f"current: [{progress_bar(current_ratio, width, is_tty)}] {pct}%")
            print(f"progress_ops: {progress} / {target} {target_note} ({pct}%)")
            print(f"progress_source: {progress_source}")
        else:
            print(f"progress_ops: {progress} (target unknown; raw_last={progress_raw_last})")
        print(f"elapsed: {format_duration(elapsed)}")
        print(f"eta: {eta}")
    else:
        print("db_bench_pid: none")
    print()
    print(f"log: {runner_log}")
    print(f"tip: tail -f {runner_log}")


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description="Realtime matrix progress watcher.")
    parser.add_argument("--matrix-dir", type=str, default="", help="Matrix root dir.")
    parser.add_argument("--refresh", type=float, default=5.0, help="Refresh interval seconds.")
    parser.add_argument("--width", type=int, default=40, help="Progress bar width.")
    parser.add_argument("--once", action="store_true", help="Print one snapshot and exit.")
    args = parser.parse_args()

    matrix_dir = Path(args.matrix_dir).resolve() if args.matrix_dir else latest_matrix_dir(root)
    if matrix_dir is None or not matrix_dir.exists():
        print("Matrix dir not found. Pass --matrix-dir.", file=sys.stderr)
        return 1

    frame = 0
    while True:
        render_once(matrix_dir, max(args.width, 10), frame)
        frame += 1
        if args.once:
            return 0

        runner_log = matrix_dir / "matrix_runner.log"
        plan_md = matrix_dir / "matrix_plan.md"
        registry_csv = matrix_dir / "run_registry.csv"
        total = count_total_runs(plan_md)
        done, skipped = count_status_runs(registry_csv)
        if not has_active_process() and total > 0 and (done + skipped) >= total and runner_log.exists():
            print()
            print("Matrix appears finished.")
            return 0

        time.sleep(max(args.refresh, 0.5))


if __name__ == "__main__":
    raise SystemExit(main())
