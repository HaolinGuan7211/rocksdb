#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


SCENARIO_ORDER = [
    "mixgraph",
    "seek200",
    "worst_seek1",
    "worst_seek4",
    "worst_seek20",
    "worst_seek200",
    "worst_seek10000",
]

REQUIRED_METRICS_COLUMNS = [
    "scenario",
    "ops_per_sec",
    "micros_per_op",
    "throughput_mb_s",
    "seek_p95_us",
    "seek_p99_us",
    "cache_hit_ratio_pct",
    "cache_hit",
    "cache_miss",
    "cache_bytes_read",
    "db_seek_count",
    "db_next_count",
    "iter_bytes_read",
    "l0_files_end",
    "cumulative_writes_count",
    "uptime_total_s",
    "operations",
]


@dataclass
class ProfileSummary:
    case_label: str
    scenario: str
    profile_tag: str
    elapsed_sec: float
    cpu_user_pct: float
    cpu_sys_pct: float
    cpu_wait_pct: float
    mem_rss_kb: float
    io_read_kb_s: float
    io_write_kb_s: float
    cswch_s: float
    nvcswch_s: float
    iostat_read_mb_s: float
    iostat_write_mb_s: float
    iostat_util_pct: float
    iostat_await_ms: float
    perf_ipc: float
    perf_cache_miss_pct: float
    perf_l1_hit_pct: float
    perf_l2_hit_pct: float
    perf_l3_hit_pct: float


def to_float(v: str) -> float:
    if v is None:
        return float("nan")
    s = str(v).strip().replace(",", "")
    if not s:
        return float("nan")
    try:
        return float(s)
    except ValueError:
        return float("nan")


def to_int(v: str) -> int:
    x = to_float(v)
    if math.isnan(x):
        return 0
    return int(x)


def read_csv(path: Path) -> List[Dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def write_csv(rows: List[Dict[str, object]], out_path: Path) -> None:
    if not rows:
        return
    out_path.parent.mkdir(parents=True, exist_ok=True)
    keys = list(rows[0].keys())
    with out_path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=keys)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def parse_env(path: Path) -> Dict[str, str]:
    out: Dict[str, str] = {}
    if not path.exists():
        return out
    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        out[k.strip()] = v.strip()
    return out


def parse_pidstat_average(path: Path, wanted: Iterable[str]) -> Dict[str, float]:
    res: Dict[str, float] = {k: float("nan") for k in wanted}
    if not path.exists():
        return res

    lines = path.read_text(encoding="utf-8", errors="ignore").splitlines()
    header: Optional[List[str]] = None
    samples: Dict[str, List[float]] = {k: [] for k in wanted}

    for line in lines:
        s = line.strip()
        if not s:
            continue
        toks = re.split(r"\s+", s)

        # Header lines look like:
        # 06:25:39 UID PID %usr %system ... Command
        if "UID" in toks and "PID" in toks and "Command" in toks:
            header = toks
            continue

        # Optional Average lines (if pidstat exits gracefully).
        if toks and toks[0] == "Average:" and header is not None:
            idx = {k: i for i, k in enumerate(header)}
            for k in wanted:
                if k in idx and idx[k] < len(toks):
                    v = to_float(toks[idx[k]])
                    if not math.isnan(v):
                        samples[k].append(v)
            continue

        # Regular sample row; requires active header.
        # Row format usually starts with HH:MM:SS then UID PID ...
        if header is None:
            continue
        if not re.match(r"^\d{2}:\d{2}:\d{2}$", toks[0]):
            continue
        if "Command" in toks:
            continue
        idx = {k: i for i, k in enumerate(header)}
        for k in wanted:
            if k in idx and idx[k] < len(toks):
                v = to_float(toks[idx[k]])
                if not math.isnan(v):
                    samples[k].append(v)

    for k in wanted:
        vals = samples.get(k, [])
        if vals:
            res[k] = sum(vals) / len(vals)
    return res


def parse_iostat(path: Path) -> Dict[str, float]:
    out = {
        "read_mb_s": float("nan"),
        "write_mb_s": float("nan"),
        "util_pct": float("nan"),
        "await_ms": float("nan"),
    }
    if not path.exists():
        return out

    lines = path.read_text(encoding="utf-8", errors="ignore").splitlines()
    header: Optional[List[str]] = None
    snapshots: List[Tuple[float, float, float, float]] = []

    for line in lines:
        s = line.strip()
        if not s:
            continue
        if s.startswith("Device"):
            header = re.split(r"\s+", s)
            continue
        if not header:
            continue
        toks = re.split(r"\s+", s)
        if len(toks) < len(header):
            continue
        if toks[0] in ("avg-cpu:", "Linux", "Device"):
            continue

        cols = {header[i]: toks[i] for i in range(min(len(header), len(toks)))}

        rmb = to_float(cols.get("rMB/s", ""))
        wmb = to_float(cols.get("wMB/s", ""))
        if math.isnan(rmb):
            rkb = to_float(cols.get("rkB/s", ""))
            rmb = rkb / 1024.0 if not math.isnan(rkb) else float("nan")
        if math.isnan(wmb):
            wkb = to_float(cols.get("wkB/s", ""))
            wmb = wkb / 1024.0 if not math.isnan(wkb) else float("nan")

        util = to_float(cols.get("%util", ""))
        await_ms = to_float(cols.get("await", ""))
        if math.isnan(await_ms):
            r_await = to_float(cols.get("r_await", ""))
            w_await = to_float(cols.get("w_await", ""))
            if not math.isnan(r_await) and not math.isnan(w_await):
                await_ms = (r_await + w_await) / 2.0
            elif not math.isnan(r_await):
                await_ms = r_await
            elif not math.isnan(w_await):
                await_ms = w_await

        if math.isnan(rmb) and math.isnan(wmb) and math.isnan(util) and math.isnan(await_ms):
            continue
        snapshots.append((rmb, wmb, util, await_ms))

    if not snapshots:
        return out

    def avg(vals: List[float]) -> float:
        good = [v for v in vals if not math.isnan(v)]
        return sum(good) / len(good) if good else float("nan")

    out["read_mb_s"] = avg([x[0] for x in snapshots])
    out["write_mb_s"] = avg([x[1] for x in snapshots])
    out["util_pct"] = avg([x[2] for x in snapshots])
    out["await_ms"] = avg([x[3] for x in snapshots])
    return out


def parse_perf_stat(path: Path) -> Dict[str, float]:
    out = {
        "ipc": float("nan"),
        "cache_miss_pct": float("nan"),
        "l1_hit_pct": float("nan"),
        "l2_hit_pct": float("nan"),
        "l3_hit_pct": float("nan"),
    }
    if not path.exists():
        return out

    event_sum: Dict[str, float] = {}
    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        s = line.strip()
        if not s or "<not" in s or "failed" in s.lower():
            continue
        toks = re.split(r"\s+", s)
        if len(toks) < 3:
            continue
        # format: <time> <value> <event> ...
        val = to_float(toks[1])
        ev = toks[2]
        if math.isnan(val):
            continue
        event_sum[ev] = event_sum.get(ev, 0.0) + val

    def ev(name: str) -> float:
        if name in event_sum:
            return event_sum[name]
        # Handle possible perf aliases/suffixes.
        for k, v in event_sum.items():
            if k.endswith(name):
                return v
        return float("nan")

    cycles = ev("cycles")
    inst = ev("instructions")
    cref = ev("cache-references")
    cmiss = ev("cache-misses")
    l1_loads = ev("L1-dcache-loads")
    l1_misses = ev("L1-dcache-load-misses")
    l2_refs = ev("l2_rqsts.references")
    l2_misses = ev("l2_rqsts.miss")
    l3_loads = ev("LLC-loads")
    l3_misses = ev("LLC-load-misses")

    if not math.isnan(cycles) and cycles > 0 and not math.isnan(inst):
        out["ipc"] = inst / cycles
    if not math.isnan(cref) and cref > 0 and not math.isnan(cmiss):
        out["cache_miss_pct"] = cmiss * 100.0 / cref
    if not math.isnan(l1_loads) and l1_loads > 0 and not math.isnan(l1_misses):
        out["l1_hit_pct"] = (1.0 - l1_misses / l1_loads) * 100.0
    if not math.isnan(l2_refs) and l2_refs > 0 and not math.isnan(l2_misses):
        out["l2_hit_pct"] = (1.0 - l2_misses / l2_refs) * 100.0
    if not math.isnan(l3_loads) and l3_loads > 0 and not math.isnan(l3_misses):
        out["l3_hit_pct"] = (1.0 - l3_misses / l3_loads) * 100.0
    return out


def load_profiles(profile_root: Optional[Path]) -> Dict[Tuple[str, str], ProfileSummary]:
    if profile_root is None or not profile_root.exists():
        return {}

    summaries: Dict[Tuple[str, str], ProfileSummary] = {}
    for env_path in profile_root.rglob("metadata.env"):
        prof_dir = env_path.parent
        meta = parse_env(env_path)
        case_label = meta.get("CASE_LABEL", "").strip()
        scenario = meta.get("SCENARIO", "").strip() or "*"
        tag = meta.get("PROFILE_TAG", prof_dir.name)

        cpu = parse_pidstat_average(prof_dir / "pidstat_cpu.log", ["%usr", "%system", "%wait"])
        mem = parse_pidstat_average(prof_dir / "pidstat_mem.log", ["RSS"])
        io = parse_pidstat_average(prof_dir / "pidstat_io.log", ["kB_rd/s", "kB_wr/s"])
        sch = parse_pidstat_average(prof_dir / "pidstat_sched.log", ["cswch/s", "nvcswch/s"])
        ios = parse_iostat(prof_dir / "iostat.log")
        perf = parse_perf_stat(prof_dir / "perf_stat.log")

        item = ProfileSummary(
            case_label=case_label,
            scenario=scenario,
            profile_tag=tag,
            elapsed_sec=to_float(meta.get("ELAPSED_SEC", "")),
            cpu_user_pct=cpu.get("%usr", float("nan")),
            cpu_sys_pct=cpu.get("%system", float("nan")),
            cpu_wait_pct=cpu.get("%wait", float("nan")),
            mem_rss_kb=mem.get("RSS", float("nan")),
            io_read_kb_s=io.get("kB_rd/s", float("nan")),
            io_write_kb_s=io.get("kB_wr/s", float("nan")),
            cswch_s=sch.get("cswch/s", float("nan")),
            nvcswch_s=sch.get("nvcswch/s", float("nan")),
            iostat_read_mb_s=ios["read_mb_s"],
            iostat_write_mb_s=ios["write_mb_s"],
            iostat_util_pct=ios["util_pct"],
            iostat_await_ms=ios["await_ms"],
            perf_ipc=perf["ipc"],
            perf_cache_miss_pct=perf["cache_miss_pct"],
            perf_l1_hit_pct=perf["l1_hit_pct"],
            perf_l2_hit_pct=perf["l2_hit_pct"],
            perf_l3_hit_pct=perf["l3_hit_pct"],
        )

        key = (case_label, scenario)
        # keep the latest by elapsed timestamp if duplicated
        prev = summaries.get(key)
        if prev is None:
            summaries[key] = item
        else:
            if item.elapsed_sec >= prev.elapsed_sec:
                summaries[key] = item

    return summaries


def pick_profile(
    profiles: Dict[Tuple[str, str], ProfileSummary], case_label: str, scenario: str
) -> Tuple[Optional[ProfileSummary], str]:
    candidates = [
        ((case_label, scenario), "exact_case_scenario"),
        ((case_label, "*"), "case_any_scenario"),
        (("*", scenario), "global_scenario"),
        (("*", "*"), "global_any_scenario"),
        (("", scenario), "empty_case_scenario"),
        (("", "*"), "empty_case_any_scenario"),
    ]
    for key, rule in candidates:
        prof = profiles.get(key)
        if prof is not None:
            return prof, rule
    return None, "none"


def missing_columns(rows: List[Dict[str, str]], required: Iterable[str]) -> List[str]:
    if not rows:
        return list(required)
    keys = set(rows[0].keys())
    return [c for c in required if c not in keys]


def scenario_key(s: str) -> int:
    if s in SCENARIO_ORDER:
        return SCENARIO_ORDER.index(s)
    return 999


def build_resource_ledger(
    matrix_dir: Path, profiles: Dict[Tuple[str, str], ProfileSummary]
) -> Tuple[List[Dict[str, object]], Dict[str, object]]:
    registry_path = matrix_dir / "run_registry.csv"
    if not registry_path.exists():
        raise FileNotFoundError(f"missing {registry_path}")

    registry = read_csv(registry_path)
    registry.sort(key=lambda r: to_int(r.get("seq", "0")))

    profile_match_breakdown: Dict[str, int] = {}
    missing_metrics_tables: List[str] = []
    missing_required_cols: Dict[str, List[str]] = {}
    done_cases = 0
    metrics_tables_found = 0
    rows_with_profile = 0
    rows_without_profile = 0
    rows: List[Dict[str, object]] = []
    for case in registry:
        if (case.get("status") or "").strip().lower() != "done":
            continue
        done_cases += 1
        case_label = (case.get("label") or "").strip()
        case_dir = Path(case["experiment_dir"])
        metrics = case_dir / "figures" / "metrics_table.csv"
        if not metrics.exists():
            missing_metrics_tables.append(str(metrics))
            continue

        metrics_rows = read_csv(metrics)
        if not metrics_rows:
            missing_metrics_tables.append(str(metrics))
            continue
        metrics_tables_found += 1

        missing = missing_columns(metrics_rows, REQUIRED_METRICS_COLUMNS)
        if missing:
            missing_required_cols[str(metrics)] = missing

        for m in metrics_rows:
            scenario = (m.get("scenario") or "").strip()
            if not scenario:
                continue

            prof, profile_match_rule = pick_profile(profiles, case_label, scenario)
            profile_match_breakdown[profile_match_rule] = profile_match_breakdown.get(profile_match_rule, 0) + 1
            if prof is None:
                rows_without_profile += 1
            else:
                rows_with_profile += 1

            ops = to_float(m.get("operations", ""))
            cache_hit = to_float(m.get("cache_hit", ""))
            cache_miss = to_float(m.get("cache_miss", ""))
            db_seek = to_float(m.get("db_seek_count", ""))
            db_next = to_float(m.get("db_next_count", ""))
            iter_bytes = to_float(m.get("iter_bytes_read", ""))

            row: Dict[str, object] = {
                "seq": to_int(case.get("seq", "0")),
                "label": case_label,
                "phase": case.get("phase", ""),
                "mode": case.get("mode", ""),
                "cache_gib": to_float(case.get("cache_gib", "")),
                "threads": to_int(case.get("threads", "0")),
                "mix_get": to_float(case.get("mix_get", "")),
                "mix_put": to_float(case.get("mix_put", "")),
                "mix_seek": to_float(case.get("mix_seek", "")),
                "scenario": scenario,
                "ops_per_sec": to_float(m.get("ops_per_sec", "")),
                "micros_per_op": to_float(m.get("micros_per_op", "")),
                "throughput_mb_s": to_float(m.get("throughput_mb_s", "")),
                "seek_p95_us": to_float(m.get("seek_p95_us", "")),
                "seek_p99_us": to_float(m.get("seek_p99_us", "")),
                "cache_hit_ratio_pct": to_float(m.get("cache_hit_ratio_pct", "")),
                "cache_hit": cache_hit,
                "cache_miss": cache_miss,
                "cache_bytes_read": to_float(m.get("cache_bytes_read", "")),
                "db_seek_count": db_seek,
                "db_next_count": db_next,
                "iter_bytes_read": iter_bytes,
                "l0_files_end": to_float(m.get("l0_files_end", "")),
                "cumulative_writes_count": to_float(m.get("cumulative_writes_count", "")),
                "uptime_total_s": to_float(m.get("uptime_total_s", "")),
                "operations": ops,
                "cache_access_total": cache_hit + cache_miss if not math.isnan(cache_hit + cache_miss) else float("nan"),
                "cache_miss_per_kop": (cache_miss * 1000.0 / ops) if ops > 0 and not math.isnan(cache_miss) else float("nan"),
                "iter_bytes_per_op": (iter_bytes / ops) if ops > 0 and not math.isnan(iter_bytes) else float("nan"),
                "next_per_seek": (db_next / db_seek) if db_seek > 0 and not math.isnan(db_next) else float("nan"),
                "frontend_cpu_ms_per_op": (to_float(m.get("micros_per_op", "")) / 1000.0),
                "profile_match_rule": profile_match_rule,
                "profile_tag": prof.profile_tag if prof else "",
                "profile_elapsed_sec": prof.elapsed_sec if prof else float("nan"),
                "cpu_user_pct": prof.cpu_user_pct if prof else float("nan"),
                "cpu_sys_pct": prof.cpu_sys_pct if prof else float("nan"),
                "cpu_wait_pct": prof.cpu_wait_pct if prof else float("nan"),
                "mem_rss_kb": prof.mem_rss_kb if prof else float("nan"),
                "io_read_kb_s": prof.io_read_kb_s if prof else float("nan"),
                "io_write_kb_s": prof.io_write_kb_s if prof else float("nan"),
                "cswch_s": prof.cswch_s if prof else float("nan"),
                "nvcswch_s": prof.nvcswch_s if prof else float("nan"),
                "iostat_read_mb_s": prof.iostat_read_mb_s if prof else float("nan"),
                "iostat_write_mb_s": prof.iostat_write_mb_s if prof else float("nan"),
                "iostat_util_pct": prof.iostat_util_pct if prof else float("nan"),
                "iostat_await_ms": prof.iostat_await_ms if prof else float("nan"),
                "perf_ipc": prof.perf_ipc if prof else float("nan"),
                "perf_cache_miss_pct": prof.perf_cache_miss_pct if prof else float("nan"),
                "perf_l1_hit_pct": prof.perf_l1_hit_pct if prof else float("nan"),
                "perf_l2_hit_pct": prof.perf_l2_hit_pct if prof else float("nan"),
                "perf_l3_hit_pct": prof.perf_l3_hit_pct if prof else float("nan"),
            }
            rows.append(row)

    rows.sort(key=lambda r: (int(r["seq"]), scenario_key(str(r["scenario"]))))
    warnings: List[str] = []
    if missing_metrics_tables:
        warnings.append(f"missing metrics_table files: {len(missing_metrics_tables)}")
    if missing_required_cols:
        warnings.append(f"metrics_table missing required columns: {len(missing_required_cols)}")
    if rows and rows_without_profile == len(rows):
        warnings.append("all rows are missing external profile data")

    validation: Dict[str, object] = {
        "matrix_dir": str(matrix_dir),
        "registry_total_cases": len(registry),
        "registry_done_cases": done_cases,
        "metrics_tables_found": metrics_tables_found,
        "missing_metrics_tables": missing_metrics_tables,
        "missing_required_columns": missing_required_cols,
        "ledger_rows": len(rows),
        "profile_records_loaded": len(profiles),
        "rows_with_profile": rows_with_profile,
        "rows_without_profile": rows_without_profile,
        "profile_match_breakdown": profile_match_breakdown,
        "warnings": warnings,
    }
    return rows, validation


def build_module_impact(rows: List[Dict[str, object]]) -> List[Dict[str, object]]:
    out: List[Dict[str, object]] = []

    for r in rows:
        base = {
            "label": r["label"],
            "threads": r["threads"],
            "scenario": r["scenario"],
        }

        def add(module: str, metric: str, value: object, unit: str, source: str) -> None:
            out.append(
                {
                    **base,
                    "module": module,
                    "metric": metric,
                    "value": value,
                    "unit": unit,
                    "source": source,
                }
            )

        add("frontend", "ops_per_sec", r["ops_per_sec"], "ops/s", "metrics_table")
        add("frontend", "cpu_ms_per_op", r["frontend_cpu_ms_per_op"], "ms/op", "metrics_table")
        add("block_cache", "hit_ratio_pct", r["cache_hit_ratio_pct"], "%", "metrics_table")
        add("block_cache", "miss_per_kop", r["cache_miss_per_kop"], "miss/1kops", "metrics_table")
        add("index_iterator", "next_per_seek", r["next_per_seek"], "next/seek", "metrics_table")
        add("sst_read", "iter_bytes_per_op", r["iter_bytes_per_op"], "B/op", "metrics_table")
        add("lsm_background", "l0_files_end", r["l0_files_end"], "count", "metrics_table")
        add("lsm_background", "cumulative_writes_count", r["cumulative_writes_count"], "count", "metrics_table")
        add("cpu_system", "cpu_user_pct", r["cpu_user_pct"], "%", "pidstat")
        add("cpu_system", "cpu_sys_pct", r["cpu_sys_pct"], "%", "pidstat")
        add("cpu_system", "cpu_wait_pct", r["cpu_wait_pct"], "%", "pidstat")
        add("memory", "rss_kb", r["mem_rss_kb"], "kB", "pidstat")
        add("io_device", "read_mb_s", r["iostat_read_mb_s"], "MB/s", "iostat")
        add("io_device", "write_mb_s", r["iostat_write_mb_s"], "MB/s", "iostat")
        add("io_device", "util_pct", r["iostat_util_pct"], "%", "iostat")
        add("io_device", "await_ms", r["iostat_await_ms"], "ms", "iostat")
        add("cpu_microarch", "ipc", r["perf_ipc"], "ratio", "perf")
        add("cpu_microarch", "cache_miss_pct", r["perf_cache_miss_pct"], "%", "perf")
        add("cpu_microarch", "l1_hit_pct", r["perf_l1_hit_pct"], "%", "perf")
        add("cpu_microarch", "l2_hit_pct", r["perf_l2_hit_pct"], "%", "perf")
        add("cpu_microarch", "l3_hit_pct", r["perf_l3_hit_pct"], "%", "perf")

    return out


def make_summary(rows: List[Dict[str, object]], out_md: Path, validation: Dict[str, object]) -> None:
    out_md.parent.mkdir(parents=True, exist_ok=True)

    mix = [r for r in rows if r["scenario"] == "mixgraph"]
    mix.sort(key=lambda x: to_int(str(x["threads"])))

    lines: List[str] = []
    lines.append("# 资源账本摘要")
    lines.append("")
    lines.append("## 范围")
    lines.append(f"- 样本行数: {len(rows)}")
    lines.append(f"- case 数: {len(set(str(r['label']) for r in rows))}")
    lines.append(f"- scenario 数: {len(set(str(r['scenario']) for r in rows))}")
    lines.append("")

    if mix:
        b_min = min(mix, key=lambda x: to_float(str(x["ops_per_sec"])))
        b_max = max(mix, key=lambda x: to_float(str(x["ops_per_sec"])))
        lines.append("## Mixgraph 线程敏感性")
        lines.append(
            f"- 吞吐最低: {b_min['label']}({b_min['threads']}t) = {to_int(str(b_min['ops_per_sec']))} ops/s"
        )
        lines.append(
            f"- 吞吐最高: {b_max['label']}({b_max['threads']}t) = {to_int(str(b_max['ops_per_sec']))} ops/s"
        )
        min_ops = to_float(str(b_min["ops_per_sec"]))
        max_ops = to_float(str(b_max["ops_per_sec"]))
        if min_ops > 0:
            lines.append(f"- 扩展增益: {(max_ops - min_ops) / min_ops * 100.0:.2f}%")
        lines.append("")

    missing_profile = [
        r
        for r in rows
        if math.isnan(to_float(str(r.get("cpu_user_pct", "nan"))))
        and math.isnan(to_float(str(r.get("iostat_util_pct", "nan"))))
    ]
    lines.append("## 数据完备性")
    if missing_profile:
        lines.append(f"- 未检测到外部 profile 采样的行数: {len(missing_profile)}")
        lines.append("- 当前账本主要基于 metrics_table 内部统计；建议后续补跑 profile_module_resource.sh 采样。")
    else:
        lines.append("- 已检测到 profile 采样数据，可进行 CPU/IO/调度归因。")
    lines.append(
        f"- profile 匹配覆盖: {validation.get('rows_with_profile', 0)}/{validation.get('ledger_rows', len(rows))} 行"
    )

    warnings = validation.get("warnings", [])
    if isinstance(warnings, list) and warnings:
        lines.append("- 校验告警:")
        for w in warnings:
            lines.append(f"  - {w}")

    lines.append("")
    lines.append("## 输出文件")
    lines.append("- resource_ledger.csv")
    lines.append("- module_impact_matrix.csv")
    lines.append("- resource_summary.md")
    lines.append("- validation.json")
    lines.append("- microarch: l1/l2/l3 hit% 字段为 best-effort（依赖 perf 事件支持）")

    out_md.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description="Aggregate RocksDB resource ledger from matrix outputs and optional profiler logs.")
    ap.add_argument("--matrix-dir", required=True, help="Matrix directory (e.g. experiment/20260206_experiment9_thread_sweep_fine_50gb)")
    ap.add_argument("--profile-root", default="", help="Optional profile root dir containing metadata.env + pidstat/iostat/perf logs")
    ap.add_argument("--output-dir", default="", help="Output directory (default: <matrix-dir>/analysis/resource_ledger)")
    ap.add_argument("--strict-validation", action="store_true", help="Fail when validation emits warnings")
    args = ap.parse_args()

    matrix_dir = Path(args.matrix_dir).resolve()
    if not matrix_dir.exists():
        raise FileNotFoundError(f"matrix dir not found: {matrix_dir}")

    if args.output_dir:
        out_dir = Path(args.output_dir).resolve()
    else:
        out_dir = matrix_dir / "analysis" / "resource_ledger"
    out_dir.mkdir(parents=True, exist_ok=True)

    profile_root = Path(args.profile_root).resolve() if args.profile_root else None
    profiles = load_profiles(profile_root)

    ledger_rows, validation = build_resource_ledger(matrix_dir, profiles)
    if not ledger_rows:
        raise RuntimeError("no resource rows generated; check matrix dir and case figures/metrics_table.csv")

    impact_rows = build_module_impact(ledger_rows)

    write_csv(ledger_rows, out_dir / "resource_ledger.csv")
    write_csv(impact_rows, out_dir / "module_impact_matrix.csv")
    make_summary(ledger_rows, out_dir / "resource_summary.md", validation)

    meta = {
        "matrix_dir": str(matrix_dir),
        "profile_root": str(profile_root) if profile_root else "",
        "rows": len(ledger_rows),
        "impact_rows": len(impact_rows),
        "output_dir": str(out_dir),
    }
    (out_dir / "meta.json").write_text(json.dumps(meta, ensure_ascii=True, indent=2), encoding="utf-8")
    (out_dir / "validation.json").write_text(json.dumps(validation, ensure_ascii=True, indent=2), encoding="utf-8")

    print(f"resource ledger generated: {out_dir}")
    print(f"rows={len(ledger_rows)}, impact_rows={len(impact_rows)}")
    warnings = validation.get("warnings", [])
    if warnings:
        print(f"validation warnings: {len(warnings)}")
        for item in warnings:
            print(f"- {item}")
        if args.strict_validation:
            raise RuntimeError("strict validation enabled and warnings found")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
