#!/usr/bin/env python3
"""
Analyze a TailProbeWriter-format CSV (tail or periodic sample) without replaying load.

This script reuses the stage attribution logic from run_tail_probe_from_matrix.py
to produce:
  - stage breakdown (absolute + share)
  - stage share distribution stats
  - stage stack plot (absolute + relative)
  - component share distribution plot

Notes:
  - The input CSV must contain the TailProbeWriter header (case_label/scenario +
    delta_* columns).
  - For periodic sampling, there is no natural "threshold_us"; we set a dummy
    value purely to satisfy the sample-row schema. Bucket-based analysis is
    intentionally omitted here.
"""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Dict, List


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples_csv", required=True, help="TailProbeWriter CSV (tail or sample)")
    ap.add_argument("--out_dir", required=True, help="directory to write outputs")
    ap.add_argument("--phase", default="probe", help="phase name used by plotting helpers")
    ap.add_argument("--scenario", default="mixgraph", help="scenario name (default: mixgraph)")
    ap.add_argument(
        "--threshold_us",
        type=int,
        default=1,
        help="dummy threshold_us to attach to rows (periodic sampling has none)",
    )
    args = ap.parse_args()

    samples_csv = Path(args.samples_csv).resolve()
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    # Reuse TailProbe analyzer logic (stage reconstruction + plots).
    import sys

    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import run_tail_probe_from_matrix as tpm  # type: ignore

    tail_rows: List[Dict[str, str]] = tpm.read_csv(samples_csv)
    if not tail_rows:
        raise SystemExit(f"empty samples: {samples_csv}")

    # Group by case_label to get per-label breakdowns.
    sample_rows: List[Dict[str, object]] = []
    for tr in tail_rows:
        label = str(tr.get("case_label", "NA") or "NA")
        sample_rows.append(
            tpm.build_sample_row(
                label=label,
                phase=str(args.phase),
                scenario=str(args.scenario),
                threshold_us=int(args.threshold_us),
                tr=tr,
            )
        )

    breakdown_rows = tpm.build_breakdown_rows(sample_rows)
    tpm.write_csv(out_dir / "stage_breakdown.csv", breakdown_rows)

    dist_rows = tpm.build_stage_distribution_rows(sample_rows)
    tpm.write_csv(out_dir / "stage_distribution.csv", dist_rows)

    # Plots (same style as tail probe). For a single phase, these are still useful.
    tpm.plot_phase_stack(
        breakdown_rows,
        phase=str(args.phase),
        out_png=out_dir / "stage_stack.png",
    )
    tpm.plot_phase_component_share_distribution(
        sample_rows,
        phase=str(args.phase),
        out_png=out_dir / "component_share_distribution.png",
    )

    print(f"[ok] wrote stage analysis under: {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

