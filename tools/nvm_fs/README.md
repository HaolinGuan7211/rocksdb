# NVM FS Module

This directory contains the NVM-oriented Env/FileSystem simulation module and its profiling assets.

## Scope

- NVM behavior simulation integrated at RocksDB Env/FileSystem layer.
- Profiling and fitting utilities for latency/bandwidth/EWR anchors.
- Independent iteration space for simulation logic, profiling scripts, and experiment docs.

## Directory Layout

- `simulated_hybrid_file_system.h`
- `simulated_hybrid_file_system.cc`
- `simulated_hybrid_file_system_test.cc`
- `simulated_doc.md`
- `envfs_profile_tool.cc`
- `run_envfs_profile.sh`
- `analyze_envfs_profile.py`
- `run_envfs_xp_bench.sh`
- `analyze_envfs_xp_bench.py`
- `WORKLOG_2026-02-07.md`

## Build Integration

Integrated paths have been redirected to this module in:

- `CMakeLists.txt`
- `src.mk`
- `BUCK`
- `tools/db_bench_tool.cc`

## Current Note

As of `2026-02-07`, `envfs_profile_tool.cc` and `run_envfs_profile.sh` in this directory are currently empty due to an interrupted/full-disk write incident and need restoration before next profiling round.
