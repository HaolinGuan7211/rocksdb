# Shortscan Experiment Framework

This framework is for stable, repeatable benchmarking before format A/B comparison.

## Scripts

- `tools/run_shortscan_compare.sh`
  - Runs one full benchmark round.
  - Default `ISOLATE_BY_CACHE=1`: each cache size uses a cloned baseline DB to avoid state drift.
- `tools/plot_shortscan_results.py`
  - Builds context-aware tables/plots (`figures/metrics_table.csv`, `figures/report.md`).
- `tools/run_shortscan_framework.sh`
  - Runs repeated rounds and aggregates medians.
- `tools/aggregate_shortscan_runs.py`
  - Aggregates metrics from multiple run dirs.

## Recommended workflow

1. Smoke validate:

```bash
PROFILE=smoke SCALE=0.0002 CACHE_SIZES=$((1<<30)) REPEATS=1 tools/run_shortscan_framework.sh
```

2. Formal S-scale framework run:

```bash
PROFILE=s SCALE=1.0 REPEATS=3 ISOLATE_BY_CACHE=1 tools/run_shortscan_framework.sh
```

3. Check outputs:
   - Per-run: `benchmark_runs/<framework_tag>_r*/figures/`
   - Aggregate: `benchmark_framework/<framework_tag>/`

## Key anti-misread rules

- Never compare cache sizes on a single mutating DB (use `ISOLATE_BY_CACHE=1`).
- Keep non-target knobs fixed.
- Use at least 3 repeats and use median for summary.
- Read performance metrics with state metrics together (`l0_files_end`, `cumulative_writes_count`).
