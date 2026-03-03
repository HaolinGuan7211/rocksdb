#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
from typing import Dict, List


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Analyze a tail probe CSV (db_bench --tail_probe_output) into stage breakdown + figures."
    )
    ap.add_argument("--samples_csv", required=True, help="tail probe samples CSV path")
    ap.add_argument("--out_dir", required=True, help="output directory")
    ap.add_argument("--label", default="case", help="label to attach to rows/plots")
    ap.add_argument("--phase", default="analysis", help="phase name (affects plot naming)")
    ap.add_argument("--scenario", default="mixgraph", help="scenario name")
    ap.add_argument("--threshold_us", type=int, default=0, help="threshold used for these samples (for reporting)")
    ap.add_argument(
        "--kind",
        default="tail",
        choices=("tail", "sample"),
        help="What these samples represent. Controls output filenames/plot suffixes.",
    )
    args = ap.parse_args()

    samples_csv = Path(args.samples_csv).resolve()
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    # Reuse the existing stage reconstruction + plots.
    import sys

    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import run_tail_probe_from_matrix as tpm  # type: ignore

    raw_rows: List[Dict[str, str]] = tpm.read_csv(samples_csv)
    sample_rows: List[Dict[str, object]] = []
    for r in raw_rows:
        sample_rows.append(
            tpm.build_sample_row(
                label=str(args.label),
                phase=str(args.phase),
                scenario=str(args.scenario),
                threshold_us=int(args.threshold_us),
                tr=r,
            )
        )

    kind = str(args.kind)

    # Core CSVs.
    tpm.write_csv(out_dir / f"{kind}_stage_samples.csv", sample_rows)
    breakdown_rows = tpm.build_breakdown_rows(sample_rows)
    tpm.write_csv(out_dir / f"{kind}_stage_breakdown.csv", breakdown_rows)
    dist_rows = tpm.build_stage_distribution_rows(sample_rows)
    tpm.write_csv(out_dir / f"{kind}_stage_distribution.csv", dist_rows)
    bucket_rows = tpm.build_latency_bucket_breakdown_rows(sample_rows)
    tpm.write_csv(out_dir / f"{kind}_latency_bucket_breakdown.csv", bucket_rows)

    # Figures.
    prefix = tpm.phase_to_output_prefix(str(args.phase))
    tpm.plot_phase_stack(
        breakdown_rows,
        phase=str(args.phase),
        out_png=out_dir / f"phase{prefix}_{kind}_stage_stack.png",
    )
    tpm.plot_phase_component_share_distribution(
        sample_rows,
        phase=str(args.phase),
        out_png=out_dir / f"phase{prefix}_{kind}_component_share_distribution.png",
    )
    tpm.plot_phase_latency_bucket_stage_stack(
        bucket_rows,
        phase=str(args.phase),
        out_png=out_dir / f"phase{prefix}_{kind}_latency_bucket_stage_stack.png",
    )

    # Paper-style filter signals.
    tpm.plot_phase_filter_total_share_distribution(
        sample_rows,
        phase=str(args.phase),
        out_png=out_dir / f"phase{prefix}_{kind}_filter_total_share_distribution.png",
    )
    tpm.plot_phase_filter_probe_vs_share_scatter(
        sample_rows,
        phase=str(args.phase),
        out_png=out_dir / f"phase{prefix}_{kind}_filter_probe_vs_share_scatter.png",
    )

    probe_summary_rows = tpm.build_filter_probe_bucket_summary_rows(
        sample_rows, phase=str(args.phase)
    )
    if probe_summary_rows:
        tpm.write_csv(out_dir / f"{kind}_filter_probe_bucket_summary.csv", probe_summary_rows)
        tpm.plot_phase_filter_probe_knee_latency(
            probe_summary_rows,
            phase=str(args.phase),
            out_png=out_dir / f"phase{prefix}_{kind}_filter_probe_knee_latency.png",
        )
        tpm.plot_phase_filter_probe_knee_filter_share(
            probe_summary_rows,
            phase=str(args.phase),
            out_png=out_dir / f"phase{prefix}_{kind}_filter_probe_knee_filter_share.png",
        )

    # Gate2: SimFS noise floor diagnostics (expected vs actual wait, overshoot, and
    # alignment between SimFS wall time and RocksDB block_read_io stage time).
    tpm.plot_phase_gate2_simfs_wait_expected_vs_actual_scatter(
        sample_rows,
        phase=str(args.phase),
        out_png=out_dir / f"phase{prefix}_{kind}_gate2_simfs_wait_expected_vs_actual.png",
    )
    tpm.plot_phase_gate2_simfs_wait_overshoot_distribution(
        sample_rows,
        phase=str(args.phase),
        out_png=out_dir / f"phase{prefix}_{kind}_gate2_simfs_wait_overshoot_distribution.png",
    )
    tpm.plot_phase_gate2_simfs_read_wall_vs_block_read_scatter(
        sample_rows,
        phase=str(args.phase),
        out_png=out_dir / f"phase{prefix}_{kind}_gate2_simfs_read_wall_vs_block_read.png",
    )

    print(f"[ok] wrote {kind} probe analysis under: {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
