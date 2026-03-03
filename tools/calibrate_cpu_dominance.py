#!/usr/bin/env python3
"""
Calibrate SimFS (simulated_hybrid_file_system) to reach a regime where
traditional RocksDB CPU factors become visible under NVM-scale media:
  - bloom/ribbon filter check CPU
  - compression CPU
  - (optionally) checksum/verify CPU (kept as a flag, not required)

Hard constraints (per user request):
  - only use mixgraph ("自研 bench / 线上特征") via EXPERIMENT_WORKFLOW runners
  - no ycsb/db_bench substitute; we drive db_bench only through repo runner
  - minimal A/B + sweep, 3 repeats median, write results under results/
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from statistics import median
from typing import Dict, List, Optional, Tuple


@dataclass(frozen=True)
class Scenario:
    name: str
    mix_get_ratio: float
    mix_put_ratio: float
    mix_seek_ratio: float


@dataclass(frozen=True)
class SweepPoint:
    tag: str  # "2x" | "1x" | "0.5x" | "MEASURE_FILTER"
    target_media_random_read_ns: float
    knob_fixed_read_overhead_ns: int
    knob_seq_read_bw_gbps: float
    knob_seq_write_bw_gbps: float
    knob_rand_bw_scale: float


@dataclass(frozen=True)
class RunConfig:
    name: str  # BASE | BF_OFF | COMP_OFF
    bloom_bits: int
    compression_type: str
    verify_checksum: bool


@dataclass(frozen=True)
class QueueKnobs:
    name: str
    xp_rpq_depth: int
    xp_rpq_parallelism: int
    xp_read_line_parallelism: int
    xp_rpq_arb_ns: int


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def _run(cmd: List[str], *, cwd: Path, env: Dict[str, str], log_path: Path) -> None:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8") as f:
        f.write("$ " + " ".join(cmd) + "\n\n")
        f.flush()
        subprocess.run(cmd, cwd=str(cwd), env=env, stdout=f, stderr=subprocess.STDOUT, check=True)


def _median(nums: List[float]) -> float:
    if not nums:
        raise ValueError("empty list")
    return float(median(nums))


def _as_float(x: str) -> Optional[float]:
    x = (x or "").strip()
    if not x:
        return None
    try:
        return float(x)
    except ValueError:
        return None


def _as_int(x: str) -> Optional[int]:
    x = (x or "").strip()
    if not x:
        return None
    try:
        return int(float(x))
    except ValueError:
        return None


def _parse_metrics_table(csv_path: Path, *, cache_bytes: int) -> Dict[str, Optional[float]]:
    if not csv_path.exists():
        raise FileNotFoundError(f"missing metrics_table.csv: {csv_path}")
    with csv_path.open("r", encoding="utf-8", newline="") as f:
        rows = list(csv.DictReader(f))
    mix_rows = [r for r in rows if (r.get("scenario") or "").strip() == "mixgraph"]
    if not mix_rows:
        raise ValueError(f"no scenario=mixgraph row in {csv_path}")
    selected = None
    for r in mix_rows:
        if _as_int(r.get("cmd_cache_size", "")) == cache_bytes:
            selected = r
            break
    if selected is None:
        selected = mix_rows[0]
    return {
        "ops_per_sec": _as_float(selected.get("ops_per_sec", "")),
        "seek_p99_us": _as_float(selected.get("seek_p99_us", "")),
    }


def _parse_perf_context_bloom_check(log_path: Path) -> Dict[str, int]:
    txt = log_path.read_text(encoding="utf-8", errors="replace")
    nanos = sum(int(x) for x in re.findall(r"\bbloom_filter_maymatch_nanos\s*=\s*(\d+)", txt))
    hit = sum(int(x) for x in re.findall(r"\bbloom_sst_hit_count\s*=\s*(\d+)", txt))
    miss = sum(int(x) for x in re.findall(r"\bbloom_sst_miss_count\s*=\s*(\d+)", txt))
    return {"bloom_filter_maymatch_nanos": nanos, "bloom_sst_hit_count": hit, "bloom_sst_miss_count": miss}


def _parse_simfs_stats_kv(path: Path) -> Dict[str, str]:
    out: Dict[str, str] = {}
    if not path.exists():
        return out
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        out[k.strip()] = v.strip()
    return out


def _ratio_throughput(off: float, on: float) -> Optional[float]:
    if on <= 0:
        return None
    return off / on


def _ratio_tail(on: Optional[float], off: Optional[float]) -> Optional[float]:
    # Tail ratio: tail(ON) / tail(OFF), larger => ON is slower.
    if on is None or off is None or off <= 0:
        return None
    return on / off


def _non_decreasing(vals: List[Optional[float]], *, eps: float) -> Optional[bool]:
    if any(v is None for v in vals):
        return None
    return (vals[1] + eps) >= vals[0] and (vals[2] + eps) >= vals[1]


def _compute_num_keys(target_db_gib: float, *, key_size: int, value_size: int) -> int:
    bytes_total = int(target_db_gib * 1024 * 1024 * 1024)
    per = max(1, int(key_size) + int(value_size))
    return max(1, bytes_total // per)


def _mix_locality_env() -> Dict[str, str]:
    # Reuse defaults from simfs mixgraph scripts.
    return {
        "MIX_KEY_DIST_A": "0.0016",
        "MIX_KEY_DIST_B": "-0.71",
        "MIX_KEYRANGE_DIST_A": "14.18",
        "MIX_KEYRANGE_DIST_B": "-2.917",
        "MIX_KEYRANGE_DIST_C": "0.0164",
        "MIX_KEYRANGE_DIST_D": "-0.08082",
        "MIX_KEYRANGE_NUM": "32",
        "MIX_ITER_K": "0.08",
        "MIX_ITER_SIGMA": "1.75",
        "MIX_ITER_THETA": "0",
        "MIX_HOTSET_ENABLE": "1",
        "MIX_HOTSET_RANGE_PCT": "0.03",
        "MIX_HOTSET_RANGE_ACCESS_PCT": "0.88",
        "MIX_HOTSET_RANGE_ZIPF_THETA": "1.0",
        "MIX_HOTSET_KEY_PCT": "0.01",
        "MIX_HOTSET_KEY_ACCESS_PCT": "0.80",
        "MIX_HOTSET_EVENLY_SPREAD_RANGES": "1",
        "MIX_SHIFT_ENABLE": "1",
        "MIX_SHIFT_MODE": "step_jump",
        "MIX_SHIFT_STAGE_SECONDS": "30",
        "MIX_SHIFT_STRIDE_RANGES": "1",
        "MIX_SHIFT_JUMP_MULTIPLIER": "4",
        "MIX_SHIFT_BASE_START_RANGE": "0",
        # burst/probe/monitor are left disabled by default for stable metrics
        "MIX_BURST_ENABLE": "0",
        "MIX_PROBE_ENABLE": "0",
        "MIX_MONITOR_ENABLE": "0",
        "SIMFS_MONITOR_ENABLE": "0",
    }


def _dimm_transfer_ns(*, bytes_: int, seq_bw_gbps: float, rand_bw_scale: float) -> float:
    # SimFS dimm model treats bw_gbps as bytes/ns (decimal GB/s).
    effective = max(1e-9, float(seq_bw_gbps) * float(rand_bw_scale))
    return max(1.0, float(bytes_) / effective)


def _choose_bandwidths_for_targets(
    *,
    media_bytes: int,
    rand_bw_scale: float,
    min_target_ns: float,
    base_seq_read_bw_gbps: float,
    base_seq_write_bw_gbps: float,
) -> Tuple[float, float, float]:
    # Ensure transfer time doesn't dominate the smallest target point.
    # Rule: transfer <= 0.25 * min_target.
    max_transfer = max(1.0, 0.25 * float(min_target_ns))
    bw_random_needed = float(media_bytes) / max_transfer
    seq_read_needed = bw_random_needed / max(1e-6, float(rand_bw_scale))
    seq_read = max(float(base_seq_read_bw_gbps), seq_read_needed)
    seq_write = max(float(base_seq_write_bw_gbps), seq_read * 0.30)
    transfer_ns = _dimm_transfer_ns(bytes_=media_bytes, seq_bw_gbps=seq_read, rand_bw_scale=rand_bw_scale)
    return seq_read, seq_write, transfer_ns


def main() -> int:
    repo_root = _repo_root()
    ap = argparse.ArgumentParser(description="Calibrate SimFS dimm-model so CPU knobs become visible under mixgraph (no ycsb/db_bench substitute).")
    ap.add_argument("--runner", default=str(repo_root / "tools" / "run_shortscan_compare.sh"))
    ap.add_argument("--db-bench", default=str(repo_root / "build" / "db_bench"))
    ap.add_argument("--out-json", default=str(repo_root / "results" / "calibration.json"))
    ap.add_argument("--runs-root", default=str(repo_root / "results" / "calibration_runs"))
    ap.add_argument("--repeats", type=int, default=3)

    ap.add_argument("--target-db-gib", type=float, default=0.10)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--fill-threads", type=int, default=0)
    ap.add_argument("--key-size", type=int, default=16)
    ap.add_argument("--value-size", type=int, default=1024)
    ap.add_argument("--cache-bytes", type=int, default=64 << 20)
    ap.add_argument("--duration-seconds", type=int, default=30)
    ap.add_argument("--ops-per-thread-cap", type=int, default=200_000_000)
    ap.add_argument("--use-direct", action="store_true", help="Use direct I/O (default: off for NVM-scale calibration).")
    ap.add_argument(
        "--tmpfs-root-base",
        default="/dev/shm",
        help="Base directory for SimFS redirect-to-tmpfs roots. Default is /tmp "
        "(writable in sandboxed environments). If you have access to /dev/shm "
        "and want true tmpfs, pass --tmpfs-root-base /dev/shm.",
    )

    ap.add_argument("--compression-on", default="zstd", choices=["none", "snappy", "lz4", "zstd"])
    ap.add_argument("--compression-off", default="none", choices=["none", "snappy", "lz4", "zstd"])
    ap.add_argument("--compression-ratio", type=float, default=1.0, help="db_bench value generator compressibility control (0..1).")
    ap.add_argument("--compression-level", type=int, default=9, help="db_bench --compression_level (for compression-on config).")
    ap.add_argument("--bloom-bits-on", type=int, default=128)
    ap.add_argument("--verify-checksum", action="store_true", help="Enable verify_checksum (default off in calibration).")

    ap.add_argument("--read-mix-get", type=float, default=0.10)
    ap.add_argument("--read-mix-put", type=float, default=0.00)
    ap.add_argument("--read-mix-seek", type=float, default=0.90)
    ap.add_argument("--read-max-scan-len", type=int, default=1)

    ap.add_argument("--rand-read-bytes", type=int, default=4096)
    ap.add_argument("--dimm-rand-bw-scale", type=float, default=0.95)
    ap.add_argument("--base-seq-read-bw-gbps", type=float, default=30.0)
    ap.add_argument("--base-seq-write-bw-gbps", type=float, default=9.2)

    ap.add_argument(
        "--storage-model",
        default="dimm",
        choices=["dimm", "xp_hybrid"],
        help="Which SimFS model to use. dimm uses --simulate_dimm_nvm=1. "
        "xp_hybrid uses --simulate_xp_nvm=1 with --simulate_xp_use_dimm_device_model=1 "
        "to keep XPBuffer locality while using DIMM-style bandwidth for miss transfers.",
    )
    ap.add_argument(
        "--sweep-mode",
        default="filter",
        choices=["filter", "paper4k"],
        help="How to generate sweep points. filter uses measured BF-check CPU cost; "
        "paper4k anchors random 4KB read latency around --paper-random-4k-ns and sweeps {2x,1x,0.5x}.",
    )
    ap.add_argument("--paper-random-4k-ns", type=float, default=1500.0, help="Target random 4KB read latency in ns (paper anchor).")
    ap.add_argument("--paper-random-4k-bytes", type=int, default=4096)
    ap.add_argument("--paper-seq-read-bw-gbps", type=float, default=12.0, help="Paper-aligned sequential read bandwidth (GB/s, decimal) used in paper4k sweep_mode.")
    ap.add_argument("--paper-seq-write-bw-gbps", type=float, default=4.0, help="Paper-aligned sequential write bandwidth (GB/s, decimal) used in paper4k sweep_mode.")
    ap.add_argument("--queue-sweep", action="store_true", help="Run an extra minimal queue/contestion sweep at the 1x point (READ_HEAVY, BASE only) to calibrate tail vs RPQ knobs.")
    ap.add_argument("--queue-rpq-depths", default="16,64", help="Comma-separated rpq_depth values for queue sweep.")
    ap.add_argument("--queue-rpq-arb-ns", default="0,500,2000,5000", help="Comma-separated per-outstanding arbitration penalty ns for queue sweep.")
    ap.add_argument("--queue-rpq-parallelism", type=int, default=1, help="RPQ parallelism used in queue sweep.")
    ap.add_argument("--queue-read-line-parallelism", type=int, default=1, help="Read line parallelism used in queue sweep.")
    ap.add_argument("--queue-sweep-threads", type=int, default=16, help="Thread count used in queue sweep runs (increases outstanding reads to make RPQ knobs visible).")
    ap.add_argument("--queue-sweep-duration-seconds", type=int, default=10, help="Duration for each queue sweep run (seconds).")

    ap.add_argument("--perf-level-filter-measure", type=int, default=6)
    ap.add_argument("--perf-level-sweep", type=int, default=1)
    ap.add_argument("--keep-tmpfs", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    runner = Path(args.runner).resolve()
    db_bench = Path(args.db_bench).resolve()
    out_json = Path(args.out_json).resolve()
    runs_root_base = Path(args.runs_root).resolve()
    out_json.parent.mkdir(parents=True, exist_ok=True)
    runs_root_base.mkdir(parents=True, exist_ok=True)

    if not runner.exists():
        raise FileNotFoundError(f"missing runner: {runner}")
    if not os.access(str(runner), os.X_OK):
        raise PermissionError(f"runner not executable: {runner}")
    if not db_bench.exists():
        raise FileNotFoundError(f"missing db_bench: {db_bench}")
    if not os.access(str(db_bench), os.X_OK):
        raise PermissionError(f"db_bench not executable: {db_bench}")

    repeats = max(1, int(args.repeats))
    fill_threads = int(args.fill_threads) if int(args.fill_threads) > 0 else int(args.threads)
    num_keys = _compute_num_keys(float(args.target_db_gib), key_size=int(args.key_size), value_size=int(args.value_size))

    read_scenario = Scenario(
        name="READ_HEAVY/mixgraph_seekheavy_shortscan",
        mix_get_ratio=float(args.read_mix_get),
        mix_put_ratio=float(args.read_mix_put),
        mix_seek_ratio=float(args.read_mix_seek),
    )
    write_scenario = Scenario(
        name="WRITE_HEAVY/mixgraph_write_dominant_shift_burst",
        mix_get_ratio=0.10,
        mix_put_ratio=0.60,
        mix_seek_ratio=0.30,
    )
    if abs((read_scenario.mix_get_ratio + read_scenario.mix_put_ratio + read_scenario.mix_seek_ratio) - 1.0) > 1e-9:
        raise ValueError("read mix ratios must sum to 1.0")
    if abs((write_scenario.mix_get_ratio + write_scenario.mix_put_ratio + write_scenario.mix_seek_ratio) - 1.0) > 1e-9:
        raise ValueError("write mix ratios must sum to 1.0")

    def simfs_args(point: SweepPoint, *, perf_level: int, tmpfs_root: Path, queue: Optional[QueueKnobs] = None) -> List[str]:
        q = queue or QueueKnobs(name="default", xp_rpq_depth=64, xp_rpq_parallelism=1, xp_read_line_parallelism=1, xp_rpq_arb_ns=0)
        common = [
            "--histogram=1",
            "--disable_wal=1",
            "--seed=12345",
            f"--perf_level={int(perf_level)}",
            "--mmap_read=1",
            "--simulate_xp_levels=0,1,2,3,4,5,6",
            "--simulate_xp_line_bytes=256",
            "--simulate_xp_service_bytes=4096",
            "--simulate_xp_buffer_bytes=16384",
            f"--simulate_xp_rpq_depth={int(q.xp_rpq_depth)}",
            "--simulate_xp_wpq_depth=64",
            f"--simulate_xp_rpq_parallelism={int(q.xp_rpq_parallelism)}",
            "--simulate_xp_wpq_parallelism=1",
            f"--simulate_xp_read_line_parallelism={int(q.xp_read_line_parallelism)}",
            "--simulate_xp_write_line_parallelism=1",
            f"--simulate_xp_rpq_arb_ns={int(q.xp_rpq_arb_ns)}",
            "--simulate_xp_wpq_submit_ns=100",
            "--simulate_xp_prefetch_hit_ns=120",
            "--simulate_xp_enable_prefetch=true",
            "--simulate_xp_redirect_to_tmpfs=1",
            f"--simulate_xp_tmpfs_root={tmpfs_root}",
            "--simulate_xp_busy_wait=true",
            f"--simulate_dimm_fixed_read_overhead_ns={int(point.knob_fixed_read_overhead_ns)}",
            "--simulate_dimm_fixed_write_overhead_ns=0",
            f"--simulate_dimm_seq_read_bw_gbps={float(point.knob_seq_read_bw_gbps):.6f}",
            f"--simulate_dimm_seq_write_bw_gbps={float(point.knob_seq_write_bw_gbps):.6f}",
            f"--simulate_dimm_rand_bw_scale={float(point.knob_rand_bw_scale):.6f}",
            "--simulate_dimm_sub_line_random_media_amp=1.0",
        ]
        if str(args.storage_model) == "dimm":
            return ["--simulate_dimm_nvm=1", *common]
        if str(args.storage_model) == "xp_hybrid":
            return [
                "--simulate_xp_nvm=1",
                "--simulate_xp_use_dimm_device_model=1",
                "--simulate_xp_dram_seq_read_ns=81",
                "--simulate_xp_dram_rand_read_ns=101",
                *common,
            ]
        raise ValueError(f"unknown storage model: {args.storage_model}")

    def run_one(
        *,
        point: SweepPoint,
        scenario: Scenario,
        cfg: RunConfig,
        rep: int,
        perf_level: int,
        run_root: Path,
        queue: Optional[QueueKnobs] = None,
        threads_override: Optional[int] = None,
        duration_override_seconds: Optional[int] = None,
    ) -> Dict[str, object]:
        qname = (queue.name if queue is not None else "default").replace("/", "_")
        run_dir = run_root / point.tag / cfg.name / scenario.name.replace("/", "_") / qname / f"r{rep}"
        run_dir.mkdir(parents=True, exist_ok=True)
        tmpfs_base = Path(str(args.tmpfs_root_base))
        tmpfs_root = tmpfs_base / f"nvm_tmpfs_root.calib_cpu.{run_root.name}.{point.tag}.{cfg.name}.{scenario.name.replace('/', '_')}.r{rep}"
        try:
            if tmpfs_root.exists():
                import shutil

                shutil.rmtree(tmpfs_root, ignore_errors=True)
            tmpfs_root.mkdir(parents=True, exist_ok=True)
        except PermissionError:
            # Fall back to /tmp to keep the runner usable in restricted
            # environments (e.g., sandbox where /dev/shm isn't writable).
            tmpfs_base = Path("/tmp")
            tmpfs_root = tmpfs_base / tmpfs_root.name
            import shutil

            shutil.rmtree(tmpfs_root, ignore_errors=True)
            tmpfs_root.mkdir(parents=True, exist_ok=True)

        extra = simfs_args(point, perf_level=perf_level, tmpfs_root=tmpfs_root, queue=queue)
        extra += [
            f"--bloom_bits={int(cfg.bloom_bits)}",
            f"--verify_checksum={'true' if cfg.verify_checksum else 'false'}",
            "--cache_index_and_filter_blocks=1",
            f"--compression_ratio={float(args.compression_ratio):.6f}",
            f"--compression_level={int(args.compression_level)}",
        ]
        if scenario.name.startswith("READ_HEAVY/") and int(args.read_max_scan_len) > 0:
            extra.append(f"--mix_max_scan_len={int(args.read_max_scan_len)}")
        extra_args = " ".join(extra)

        env = dict(os.environ)
        env.update(_mix_locality_env())
        env.update(
            {
                "DB_BENCH": str(db_bench),
                "OUT_DIR": str(run_dir),
                "EXPERIMENT_DIR": str(run_dir),
                "PROFILE": "smoke",
                "SCALE": "1.0",
                "CACHE_SIZES": str(int(args.cache_bytes)),
                "THREADS": str(int(threads_override) if threads_override is not None else int(args.threads)),
                "FILL_THREADS": str(int(fill_threads)),
                "KEY_SIZE": str(int(args.key_size)),
                "VALUE_SIZE": str(int(args.value_size)),
                "COMPRESSION_TYPE": str(cfg.compression_type),
                "USE_DIRECT": "true" if args.use_direct else "false",
                "NUM_KEYS": str(int(num_keys)),
                "SKIP_FILL": "0",
                "FILL_BENCHMARK": "fillseq",
                "ISOLATE_BY_CACHE": "0",
                "RUN_ONLY_MIXGRAPH": "1",
                "AUTO_POST_PROCESS": "1",
                "CLEAR_OS_CACHE_BETWEEN_STEPS": "0",
                "EXTRA_DB_BENCH_ARGS": extra_args,
                "SIMFS_STATS_DIR": str(run_dir / "simfs_stats"),
                "DB_DIR": str(run_dir / "db"),
                "WAL_DIR": str(run_dir / "wal"),
                "REALISTIC_READS": str(int(args.ops_per_thread_cap)),
                "MIXGRAPH_DURATION_SECONDS": str(int(duration_override_seconds) if duration_override_seconds is not None else int(args.duration_seconds)),
                "MIX_GET_RATIO": f"{scenario.mix_get_ratio:.6f}",
                "MIX_PUT_RATIO": f"{scenario.mix_put_ratio:.6f}",
                "MIX_SEEK_RATIO": f"{scenario.mix_seek_ratio:.6f}",
                # READ_HEAVY control knobs to reduce compaction/I/O interference.
                "POST_FILL_BENCHMARKS": "compactall,waitforcompaction" if scenario.name.startswith("READ_HEAVY/") else "",
                "MIXGRAPH_READONLY": "1" if scenario.name.startswith("READ_HEAVY/") and scenario.mix_put_ratio == 0.0 else "0",
                "MIXGRAPH_DISABLE_AUTO_COMPACTIONS": "1" if scenario.name.startswith("READ_HEAVY/") and scenario.mix_put_ratio == 0.0 else "0",
            }
        )

        driver_log = run_dir / "driver.log"
        try:
            _run(["bash", str(runner)], cwd=repo_root, env=env, log_path=driver_log)
        finally:
            if not args.keep_tmpfs:
                import shutil

                shutil.rmtree(tmpfs_root, ignore_errors=True)

        metrics = _parse_metrics_table(run_dir / "figures" / "metrics_table.csv", cache_bytes=int(args.cache_bytes))
        out: Dict[str, object] = {
            "run_dir": str(run_dir),
            "ops_per_sec": metrics.get("ops_per_sec"),
            "seek_p99_us": metrics.get("seek_p99_us"),
            "queue": None if queue is None else {
                "name": queue.name,
                "xp_rpq_depth": int(queue.xp_rpq_depth),
                "xp_rpq_parallelism": int(queue.xp_rpq_parallelism),
                "xp_read_line_parallelism": int(queue.xp_read_line_parallelism),
                "xp_rpq_arb_ns": int(queue.xp_rpq_arb_ns),
            },
        }

        mix_log = run_dir / f"02_mixgraph_cache_{int(args.cache_bytes)}.log"
        if mix_log.exists():
            out["perf_context"] = _parse_perf_context_bloom_check(mix_log)

        simfs_stats = run_dir / "simfs_stats" / f"02_mixgraph_cache_{int(args.cache_bytes)}.simfs_stats.txt"
        out["simfs_stats_path"] = str(simfs_stats) if simfs_stats.exists() else None
        if simfs_stats.exists():
            kv = _parse_simfs_stats_kv(simfs_stats)
            out["simfs_stats"] = kv
            try:
                read_ops = float(kv.get("read_ops", "0") or "0")
                media_read_bytes = float(kv.get("media_read_bytes", "0") or "0")
                sim_read_delay_ns = float(kv.get("simulated_read_delay_ns", "0") or "0")
                sim_read_media_ns = float(kv.get("simulated_read_media_delay_ns", "0") or "0")
                out["simfs_derived"] = {
                    "avg_media_bytes_per_read": (media_read_bytes / read_ops) if read_ops > 0 else None,
                    "avg_simulated_read_delay_ns_per_read": (sim_read_delay_ns / read_ops) if read_ops > 0 else None,
                    "avg_simulated_read_media_delay_ns_per_read": (sim_read_media_ns / read_ops) if read_ops > 0 else None,
                }
            except Exception:
                pass
        return out

    ts = time.strftime("%Y%m%d_%H%M%S")
    run_root = runs_root_base / ts
    run_root.mkdir(parents=True, exist_ok=True)

    # Phase B: measure BF-check CPU cost using perf_context.
    measure_point = SweepPoint(
        tag="MEASURE_FILTER",
        target_media_random_read_ns=0.0,
        knob_fixed_read_overhead_ns=1,
        knob_seq_read_bw_gbps=1000.0,
        knob_seq_write_bw_gbps=300.0,
        knob_rand_bw_scale=float(args.dimm_rand_bw_scale),
    )
    base_cfg = RunConfig(name="BASE", bloom_bits=int(args.bloom_bits_on), compression_type=str(args.compression_on), verify_checksum=bool(args.verify_checksum))

    t_filter_samples: List[float] = []
    t_filter_debug: List[Dict[str, object]] = []
    for rep in range(1, repeats + 1):
        r = run_one(point=measure_point, scenario=read_scenario, cfg=base_cfg, rep=rep, perf_level=int(args.perf_level_filter_measure), run_root=run_root)
        perf = r.get("perf_context", {}) or {}
        nanos = int(perf.get("bloom_filter_maymatch_nanos", 0))
        hit = int(perf.get("bloom_sst_hit_count", 0))
        miss = int(perf.get("bloom_sst_miss_count", 0))
        checks = hit + miss
        if checks <= 0 or nanos <= 0:
            raise RuntimeError(f"failed to collect bloom check perf counters (nanos={nanos}, checks={checks}) in {r.get('run_dir')}")
        t_ns = float(nanos) / float(checks)
        t_filter_samples.append(t_ns)
        t_filter_debug.append({"run_dir": r.get("run_dir"), "bloom_filter_maymatch_nanos": nanos, "bloom_checks": checks, "ns_per_check": t_ns})
    t_filter_check_ns = _median(t_filter_samples)

    targets = [("2x", 2.0), ("1x", 1.0), ("0.5x", 0.5)]
    if str(args.sweep_mode) == "filter":
        anchor_ns = float(t_filter_check_ns)
    else:
        anchor_ns = float(args.paper_random_4k_ns)
    min_target_ns = min(mult * anchor_ns for _, mult in targets)
    rand_bw_scale = float(args.dimm_rand_bw_scale)
    if str(args.sweep_mode) == "paper4k":
        seq_read_bw = float(args.paper_seq_read_bw_gbps)
        seq_write_bw = float(args.paper_seq_write_bw_gbps)
        transfer_ns = _dimm_transfer_ns(
            bytes_=int(args.paper_random_4k_bytes),
            seq_bw_gbps=seq_read_bw,
            rand_bw_scale=rand_bw_scale,
        )
    else:
        seq_read_bw, seq_write_bw, transfer_ns = _choose_bandwidths_for_targets(
            media_bytes=int(args.rand_read_bytes),
            rand_bw_scale=rand_bw_scale,
            min_target_ns=min_target_ns,
            base_seq_read_bw_gbps=float(args.base_seq_read_bw_gbps),
            base_seq_write_bw_gbps=float(args.base_seq_write_bw_gbps),
        )

    sweep_points: List[SweepPoint] = []
    for tag, mult in targets:
        target_total_ns = float(mult) * anchor_ns
        if str(args.sweep_mode) == "paper4k":
            if str(args.storage_model) == "xp_hybrid":
                # For XP-hybrid, total random 4KB read is approximately:
                # fixed_overhead + xp_latency_ns + transfer(4KB,bw_random) + dram_rand.
                xp_latency_ns = 300.0
                dram_rand_ns = 101.0
                fixed_ns = int(max(0.0, target_total_ns - transfer_ns - xp_latency_ns - dram_rand_ns))
            else:
                fixed_ns = int(max(0.0, target_total_ns - transfer_ns))
        else:
            fixed_ns = int(max(1.0, target_total_ns - transfer_ns))
        sweep_points.append(
            SweepPoint(
                tag=tag,
                target_media_random_read_ns=target_total_ns,
                knob_fixed_read_overhead_ns=fixed_ns,
                knob_seq_read_bw_gbps=seq_read_bw,
                knob_seq_write_bw_gbps=seq_write_bw,
                knob_rand_bw_scale=rand_bw_scale,
            )
        )

    # Phase C: A/B sweep (READ_HEAVY, 3 repeats median).
    cfgs = {
        "BASE": base_cfg,
        "BF_OFF": RunConfig(name="BF_OFF", bloom_bits=0, compression_type=base_cfg.compression_type, verify_checksum=base_cfg.verify_checksum),
        "COMP_OFF": RunConfig(name="COMP_OFF", bloom_bits=base_cfg.bloom_bits, compression_type=str(args.compression_off), verify_checksum=base_cfg.verify_checksum),
    }

    sweep_results: Dict[str, Dict[str, Dict[str, object]]] = {}
    for point in sweep_points:
        sweep_results[point.tag] = {}
        for cfg_name, cfg in cfgs.items():
            runs: List[Dict[str, object]] = []
            ops_list: List[float] = []
            p99_list: List[float] = []
            for rep in range(1, repeats + 1):
                r = run_one(point=point, scenario=read_scenario, cfg=cfg, rep=rep, perf_level=int(args.perf_level_sweep), run_root=run_root)
                runs.append(r)
                if r.get("ops_per_sec") is not None:
                    ops_list.append(float(r["ops_per_sec"]))
                if r.get("seek_p99_us") is not None:
                    p99_list.append(float(r["seek_p99_us"]))
            if not ops_list:
                raise RuntimeError(f"missing ops_per_sec for point={point.tag} cfg={cfg_name}")
            sweep_results[point.tag][cfg_name] = {
                "median_ops_per_sec": _median(ops_list),
                "median_seek_p99_us": _median(p99_list) if p99_list else None,
                "runs": runs,
            }

    def _med(point_tag: str, cfg_name: str) -> Tuple[float, Optional[float]]:
        return float(sweep_results[point_tag][cfg_name]["median_ops_per_sec"]), sweep_results[point_tag][cfg_name]["median_seek_p99_us"]

    # A/B ratios per point.
    ab_bf = []
    ab_comp = []
    bf_effect: List[Optional[float]] = []
    comp_effect: List[Optional[float]] = []
    for point in sweep_points:
        base_ops, base_p99 = _med(point.tag, "BASE")
        bf_ops, bf_p99 = _med(point.tag, "BF_OFF")
        comp_ops, comp_p99 = _med(point.tag, "COMP_OFF")

        bf_tput_ratio = _ratio_throughput(bf_ops, base_ops)
        bf_tail_ratio = _ratio_tail(base_p99, bf_p99)
        comp_tput_ratio = _ratio_throughput(comp_ops, base_ops)
        comp_tail_ratio = _ratio_tail(base_p99, comp_p99)

        ab_bf.append({"point": point.tag, "throughput_ratio": bf_tput_ratio, "tail_ratio": bf_tail_ratio})
        ab_comp.append({"point": point.tag, "throughput_ratio": comp_tput_ratio, "tail_ratio": comp_tail_ratio})
        bf_effect.append(max([v for v in [bf_tput_ratio, bf_tail_ratio] if v is not None], default=None))
        comp_effect.append(max([v for v in [comp_tput_ratio, comp_tail_ratio] if v is not None], default=None))

    def _max_ratio(entries: List[Dict[str, object]]) -> float:
        m = 0.0
        for e in entries:
            for k in ("throughput_ratio", "tail_ratio"):
                v = e.get(k)
                if v is not None:
                    m = max(m, float(v))
        return m

    bf_max = _max_ratio(ab_bf)
    comp_max = _max_ratio(ab_comp)
    sig = max(bf_max, comp_max) >= 1.2
    bf_trend = _non_decreasing(bf_effect, eps=0.03)
    comp_trend = _non_decreasing(comp_effect, eps=0.03)
    trend_ok = (bf_trend is True) or (comp_trend is True)
    passed = bool(sig and trend_ok)

    notes: List[str] = []
    if not sig:
        notes.append(f"FAIL: no A/B ratio reached 1.2 (bf_max={bf_max:.3f}, comp_max={comp_max:.3f}).")
    if not trend_ok:
        notes.append("FAIL: A/B impact does not show non-decreasing trend as media gets faster (2x->1x->0.5x).")
    if passed:
        notes.append("PASS: at least one CPU knob (BF or compression) shows >=1.2x sensitivity and does not weaken as media gets faster.")

    # Phase D: bandwidth sanity on WRITE_HEAVY at fastest point.
    fastest = sweep_points[-1]
    bw_runs: List[Dict[str, object]] = []
    bw_read_gbps: List[float] = []
    bw_write_gbps: List[float] = []
    for rep in range(1, repeats + 1):
        r = run_one(point=fastest, scenario=write_scenario, cfg=base_cfg, rep=rep, perf_level=int(args.perf_level_sweep), run_root=run_root)
        bw_runs.append(r)
        stats_path = Path(str(r.get("simfs_stats_path") or ""))
        if stats_path.exists():
            kv = _parse_simfs_stats_kv(stats_path)
            mr = float(kv.get("media_read_bytes", "0") or "0")
            mw = float(kv.get("media_write_bytes", "0") or "0")
            rd = float(kv.get("simulated_read_media_delay_ns", "0") or "0")
            wd = float(kv.get("simulated_write_media_delay_ns", "0") or "0")
            if rd > 0:
                bw_read_gbps.append(mr / rd)  # bytes/ns == GB/s
            if wd > 0:
                bw_write_gbps.append(mw / wd)

    # Optional: queue/contestion sweep at the anchor point (1x) to help
    # calibrate tail latency behavior without changing bandwidth.
    queue_sweep_out = None
    if bool(args.queue_sweep):
        def _parse_int_list(s: str) -> List[int]:
            out = []
            for tok in (s or "").split(","):
                tok = tok.strip()
                if not tok:
                    continue
                out.append(int(tok))
            return out

        depths = _parse_int_list(str(args.queue_rpq_depths))
        arbs = _parse_int_list(str(args.queue_rpq_arb_ns))
        q_knobs: List[QueueKnobs] = []
        for d in depths:
            for a in arbs:
                q_knobs.append(
                    QueueKnobs(
                        name=f"rpq{d}_arb{a}",
                        xp_rpq_depth=int(d),
                        xp_rpq_parallelism=int(args.queue_rpq_parallelism),
                        xp_read_line_parallelism=int(args.queue_read_line_parallelism),
                        xp_rpq_arb_ns=int(a),
                    )
                )
        anchor_point = next((p for p in sweep_points if p.tag == "1x"), sweep_points[1])
        q_results = []
        for q in q_knobs:
            ops_list: List[float] = []
            p99_list: List[float] = []
            runs = []
            for rep in range(1, repeats + 1):
                r = run_one(
                    point=anchor_point,
                    scenario=read_scenario,
                    cfg=base_cfg,
                    rep=rep,
                    perf_level=int(args.perf_level_sweep),
                    run_root=run_root,
                    queue=q,
                    threads_override=int(args.queue_sweep_threads),
                    duration_override_seconds=int(args.queue_sweep_duration_seconds),
                )
                runs.append(r)
                if r.get("ops_per_sec") is not None:
                    ops_list.append(float(r["ops_per_sec"]))
                if r.get("seek_p99_us") is not None:
                    p99_list.append(float(r["seek_p99_us"]))
            q_results.append(
                {
                    "queue": {
                        "name": q.name,
                        "xp_rpq_depth": q.xp_rpq_depth,
                        "xp_rpq_parallelism": q.xp_rpq_parallelism,
                        "xp_read_line_parallelism": q.xp_read_line_parallelism,
                        "xp_rpq_arb_ns": q.xp_rpq_arb_ns,
                    },
                    "median_ops_per_sec": _median(ops_list) if ops_list else None,
                    "median_seek_p99_us": _median(p99_list) if p99_list else None,
                    "runs": runs,
                }
            )
        queue_sweep_out = {
            "point": anchor_point.tag,
            "note": "Queue sweep varies RPQ depth and per-outstanding arbitration penalty at fixed bandwidth/4KB anchor; intended to shape tail latency without changing media bandwidth.",
            "results": q_results,
        }

    out = {
        "bench": {"read": read_scenario.name, "write": write_scenario.name},
        "t_filter_check": {
            "value": t_filter_check_ns,
            "unit": "ns",
            "how_measured": "PerfContext bloom_filter_maymatch_nanos / (bloom_sst_hit_count + bloom_sst_miss_count) in mixgraph logs (perf_level=6), median over repeats.",
            "raw_runs": t_filter_debug,
        },
        "simfs_knob": {
            "name": "simulate_dimm_fixed_read_overhead_ns",
            "semantic": "Per-read-request fixed overhead in dimm model; added before dimm read queue scheduling/transfer. Affects reads; writes use simulate_dimm_fixed_write_overhead_ns.",
            "mapping_to_media_read": f"T_media_random_read({int(args.rand_read_bytes)}B) ~= fixed_read_overhead_ns + {int(args.rand_read_bytes)}/(seq_read_bw_gbps*rand_bw_scale)  (bw is bytes/ns; see tools/nvm_fs/simulated_hybrid_file_system.cc: SimulateDimmReadQueueServe).",
        },
        "paper_anchor": {
            "enabled": (str(args.sweep_mode) == "paper4k"),
            "paper_random_4k_read_ns": float(args.paper_random_4k_ns),
            "paper_random_4k_bytes": int(args.paper_random_4k_bytes),
            "paper_seq_read_bw_gbps": float(args.paper_seq_read_bw_gbps),
            "paper_seq_write_bw_gbps": float(args.paper_seq_write_bw_gbps),
            "storage_model": str(args.storage_model),
        },
        "sweep_points": [
            {
                "target": p.tag,
                "target_media_random_read_ns": p.target_media_random_read_ns,
                "knob_value": {
                    "simulate_dimm_fixed_read_overhead_ns": p.knob_fixed_read_overhead_ns,
                    "simulate_dimm_seq_read_bw_gbps": p.knob_seq_read_bw_gbps,
                    "simulate_dimm_seq_write_bw_gbps": p.knob_seq_write_bw_gbps,
                    "simulate_dimm_rand_bw_scale": p.knob_rand_bw_scale,
                },
                "results": sweep_results[p.tag],
            }
            for p in sweep_points
        ],
        "ab": {"bf": ab_bf, "compression": ab_comp},
        "bandwidth_sanity": {
            "point": fastest.tag,
            "estimated_media_read_gbps_median": _median(bw_read_gbps) if bw_read_gbps else None,
            "estimated_media_write_gbps_median": _median(bw_write_gbps) if bw_write_gbps else None,
            "note": "Derived from simfs per-step stats: media_{read,write}_bytes / simulated_{read,write}_media_delay_ns for WRITE_HEAVY mixgraph step.",
            "runs": bw_runs,
        },
        "queue_sweep": queue_sweep_out,
        "pass": passed,
        "notes": notes,
        "artifacts": {
            "runs_root": str(run_root),
            "out_json": str(out_json),
            "runner": str(runner),
            "db_bench": str(db_bench),
            "settings": {
                "target_db_gib": float(args.target_db_gib),
                "num_keys": int(num_keys),
                "threads": int(args.threads),
                "fill_threads": int(fill_threads),
                "cache_bytes": int(args.cache_bytes),
                "duration_seconds": int(args.duration_seconds),
                "ops_per_thread_cap": int(args.ops_per_thread_cap),
                "compression_on": str(args.compression_on),
                "compression_off": str(args.compression_off),
                "compression_ratio": float(args.compression_ratio),
                "compression_level": int(args.compression_level),
                "bloom_bits_on": int(args.bloom_bits_on),
                "verify_checksum": bool(args.verify_checksum),
                "rand_read_bytes": int(args.rand_read_bytes),
                "base_seq_read_bw_gbps": float(args.base_seq_read_bw_gbps),
                "base_seq_write_bw_gbps": float(args.base_seq_write_bw_gbps),
                "chosen_seq_read_bw_gbps": float(seq_read_bw),
                "chosen_seq_write_bw_gbps": float(seq_write_bw),
                "chosen_transfer_ns_at_rand_read_bytes": float(transfer_ns),
                "dimm_rand_bw_scale": float(args.dimm_rand_bw_scale),
                "read_mix_get": float(args.read_mix_get),
                "read_mix_put": float(args.read_mix_put),
                "read_mix_seek": float(args.read_mix_seek),
                "read_max_scan_len": int(args.read_max_scan_len),
            },
        },
    }

    out_json.write_text(json.dumps(out, indent=2, sort_keys=False), encoding="utf-8")
    print(f"Wrote: {out_json}")
    print(f"Runs:  {run_root}")
    print(f"PASS={passed}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
