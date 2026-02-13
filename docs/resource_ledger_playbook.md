# 资源账本落地手册（v1）

本文档定义如何在现有 benchmark 工作流上，建立“可归因、可对账、可持续”的资源账本。

## 1. 目标

- 不只看吞吐/延迟，而是回答：每个模块吃了多少资源、为何吃、是否值得。
- 资源维度覆盖：CPU、内存、I/O、并发等待、微架构效率。
- 产物可直接用于实验对比（A/B 格式、参数 sweep、线程 sweep）。
- 实验 phase 口径固定：
  - A 类：`cache_sweep`（block cache size）
  - B 类：`thread_sweep`（线程数）
  - C 类：`locality_sweep`（get/seek 混比与 locality）

## 2. 模块口径（建议统一）

- `frontend`: 请求执行路径总体成本（ops、micros/op、tail latency）。
- `block_cache`: 命中率、miss 密度、cache 读字节。
- `index_iterator`: seek/next 行为比（`next_per_seek`）反映索引与迭代跳转负载。
- `sst_read`: 迭代读字节与读放大 proxy（`iter_bytes_per_op`）。
- `lsm_background`: L0 文件、累计写入计数，反映后台写债务。
- `cpu_system`: user/sys/wait CPU、上下文切换。
- `io_device`: 设备读写带宽、util、await。
- `cpu_microarch`: IPC、cache miss%。

## 3. 采集脚本

### 3.1 新增脚本

- `tools/profile_module_resource.sh`

功能：

- 采集 `pidstat`（CPU/mem/IO/sched）
- 采集 `iostat`（系统设备层）
- 采集 `perf stat`（若可稳定绑定 PID）
- 统一输出 `metadata.env`

### 3.2 常用用法

1. 对单条命令采样：

```bash
tools/profile_module_resource.sh \
  --out-dir /tmp/profile_bench_B2_mixgraph \
  --case-label B2 \
  --scenario mixgraph \
  --target-name db_bench \
  -- bash -lc '...你的 db_bench 命令...'
```

2. 对已运行 PID 采样固定时长：

```bash
tools/profile_module_resource.sh \
  --out-dir /tmp/profile_pid_12345 \
  --target-pid 12345 \
  --duration 120
```

3. 包裹矩阵脚本采样（推荐先这样做）：

```bash
tools/profile_module_resource.sh \
  --out-dir /tmp/profile_matrix_exp9 \
  --target-name db_bench \
  -- bash -lc 'EXP_DATE=20260206 ... tools/run_standard_matrix_50gb.sh'
```

### 3.3 与标准矩阵脚本集成（推荐）

`tools/run_standard_matrix_50gb.sh` 已支持资源账本自动接入：

- `ENABLE_RESOURCE_LEDGER=1`（默认）：矩阵结束后自动生成 `analysis/resource_ledger/*`
- `ENABLE_RESOURCE_LEDGER_PLOTS=0`（默认）：旧版横向占用图默认关闭
- `ENABLE_READPATH_TIMELINE=1`（默认）：自动生成读路径事件时序与归因图
- `ENABLE_TAIL_PROBE=1`（默认）：自动生成长尾探针（phase 内嵌，不独立）
- `TAIL_PROBE_CASES=A2,B2,C2`：可显式指定长尾探针 case，默认自动选 A2/B2/C2
- `TAIL_PROBE_STEP_IDS=02`：默认仅对 mixgraph 做尾样本采样
- `TAIL_PROBE_BASELINE_RUNS=5`：尾阈值估计重复次数
- `TAIL_PROBE_THRESHOLD_MULTIPLIER=1.0`：阈值倍率（基于 baseline median p99）
- `TAIL_PROBE_THRESHOLD_MIN_US=1000`：阈值下限（微秒）
- `TAIL_PROBE_MAX_SAMPLES=20000`：每个 case 的最大尾样本数
- `ENABLE_CASE_PROFILE=1`：按 case 自动调用 `profile_module_resource.sh`
- `CASE_PROFILE_ROOT=/path/to/profiles`：指定 case profile 输出目录
- `RESOURCE_LEDGER_PROFILE_ROOT=/path/to/profiles`：聚合账本时指定 profile 输入根目录（优先级高于 `CASE_PROFILE_ROOT`）
- `RESOURCE_LEDGER_STRICT=1`：账本生成失败时直接 fail（默认 0 为告警继续）

示例：

```bash
EXP_DATE=20260206 \
EXPERIMENT_ID=10 \
PHASE_FILTER=thread_sweep \
ENABLE_CASE_PROFILE=1 \
CASE_PROFILE_ROOT=/tmp/profile_exp10_case \
tools/run_standard_matrix_50gb.sh
```

## 4. 聚合脚本

### 4.1 新增脚本

- `tools/aggregate_resource_ledger.py`
- `tools/plot_resource_ledger.py`

输入：

- `--matrix-dir`: 必填，矩阵目录
- `--profile-root`: 可选，profile 采样目录根

输出（默认到 `<matrix-dir>/analysis/resource_ledger/`）：

- `resource_ledger.csv`
- `module_impact_matrix.csv`
- `resource_summary.md`
- `meta.json`
- `validation.json`
- `readpath_event_samples.csv`（自动抽取全部 `rocksdb.*` 事件）
- `event_metric_relevance.csv`（事件-性能参数相关性筛选）
- `readpath_timeline_events.csv`（仅保留相关事件的时序链路）
- `phaseA_readpath_time_stage_stack.png`（存在 A phase 时）
- `phaseB_readpath_time_stage_stack.png`（存在 B phase 时）
- `phaseC_readpath_time_stage_stack.png`（存在 C phase 时）
- `phaseA_readpath_bandwidth_stage_stack.png`（存在 A phase 时）
- `phaseB_readpath_bandwidth_stage_stack.png`（存在 B phase 时）
- `phaseC_readpath_bandwidth_stage_stack.png`（存在 C phase 时）
- `phaseA_readpath_cpu_stage_stack.png`（存在 A phase 且 CPU profile 可用时）
- `phaseB_readpath_cpu_stage_stack.png`（存在 B phase 且 CPU profile 可用时）
- `phaseC_readpath_cpu_stage_stack.png`（存在 C phase 且 CPU profile 可用时）
- `event_relevance_heatmap.png`
- `readpath_timeline_report.md`
- `tail_probe/tail_threshold_summary.csv`
- `tail_probe/tail_stage_samples.csv`
- `tail_probe/tail_stage_breakdown.csv`
- `tail_probe/tail_probe_report.md`
- `tail_probe/phaseA_tail_seek_stage_stack.png`（存在 A phase 且有 tail 样本时）
- `tail_probe/phaseB_tail_seek_stage_stack.png`（存在 B phase 且有 tail 样本时）
- `tail_probe/phaseC_tail_seek_stage_stack.png`（存在 C phase 且有 tail 样本时）

### 4.2 用法示例

1. 仅基于已有 metrics_table 聚合：

```bash
python3 tools/aggregate_resource_ledger.py \
  --matrix-dir experiment/20260206_experiment9_thread_sweep_fine_50gb
```

2. 叠加 profile 日志归因：

```bash
python3 tools/aggregate_resource_ledger.py \
  --matrix-dir experiment/20260206_experiment9_thread_sweep_fine_50gb \
  --profile-root /tmp/profile_matrix_exp9
```

3. 严格校验模式（有告警则失败）：

```bash
python3 tools/aggregate_resource_ledger.py \
  --matrix-dir experiment/20260206_experiment9_thread_sweep_fine_50gb \
  --strict-validation
```

4. 仅重画资源账本图像：

```bash
python3 tools/plot_resource_ledger.py \
  --matrix-dir experiment/20260206_experiment9_thread_sweep_fine_50gb
```

5. 生成读路径时序与归因图（推荐）：

```bash
python3 tools/analyze_readpath_timeline.py \
  --matrix-dir experiment/20260206_experiment9_thread_sweep_fine_50gb
```

## 4.3 读路径归因规则（强制建议）

- 从 run log 自动抽取全部 `rocksdb.*` 事件，不做人工白名单裁剪。
- 阶段映射后按时序输出，关注 `memtable_route`、`bloom_filter`、`table_open_meta`、`index_lookup`、`seek_dispatch`、`block_read_io`、`block_decode_checksum`、`block_cache_insert`、`iter_merge_jump` 等。
- `index_lookup` 与 `table_open_meta` 必须分离统计，禁止把 `table.open.*` 计入 `index_lookup`。
- 相关性筛选默认使用 Spearman：`|rho| >= 0.45` 且 `n >= 6`。
- 正式结论图必须同时包含绝对值与相对占比（absolute + share）。
- 三条通道定义：
  - `time`：`us/op` 的时间成本分摊
  - `bandwidth`：`MB/s` 的带宽成本分摊
  - `cpu`：`cpu pct points` 的 CPU 成本分摊（仅纳入与 `cpu_total_pct` 相关事件）
- 与目标资源无显著关系的事件不纳入该通道（例如仅计算相关而与 I/O 无关时，不计入 I/O 归因）。

## 4.4 CPU 缺失补采（操作规范）

- 触发条件：`resource_ledger.csv` 中目标 case 的 `cpu_user_pct/cpu_sys_pct` 为 `nan`。
- 推荐补采命令（先补 mixgraph）：

```bash
tools/replay_profile_from_matrix_cmds.sh \
  --matrix-dir experiment/<exp_dir> \
  --profile-root /tmp/profile_matrix_<exp_tag> \
  --cases A1,A3 \
  --steps 02 \
  --max-runs 2
```

- 补采后必须带 `--profile-root` 重聚合：

```bash
python3 tools/aggregate_resource_ledger.py \
  --matrix-dir experiment/<exp_dir> \
  --profile-root /tmp/profile_matrix_<exp_tag>
```

- 再重跑读路径分析并校验：`lane=cpu` 分阶段绝对值之和应与该 case 的 `cpu_total_pct` 对齐（允许小数误差）。

## 4.5 长尾探针（tail probe）

- 设计原则：
  - 宏观账本用于衡量尾部长度（如 `seek_p99_us`），不直接替代尾样本内部延时构成分析。
  - 长尾探针属于 A/B/C phase 的转向流程，不是独立实验类型。
  - 先做重复 baseline 获取置信的 p99，再据此设置 tail 阈值，仅采样尾部查询。
- 自动执行（矩阵结束后）：
  - 由 `tools/run_standard_matrix_50gb.sh` 调用 `tools/run_tail_probe_from_matrix.py`。
- 手动执行：

```bash
python3 tools/run_tail_probe_from_matrix.py \
  --matrix-dir experiment/<exp_dir> \
  --cases A2,B2,C2 \
  --step-ids 02 \
  --baseline-runs 5 \
  --threshold-multiplier 1.0 \
  --threshold-min-us 1000 \
  --max-samples 20000
```

- 关键输出：
  - `tail_threshold_summary.csv`：每个 case 的 baseline 置信统计、阈值、采样状态。
  - `tail_stage_samples.csv`：逐 tail 样本的阶段耗时、I/O wait proxy、读写字节等。
  - `tail_stage_breakdown.csv`：按 case 聚合后的阶段绝对值与占比。
  - `phaseX_tail_seek_stage_stack.png`：按 phase 的长尾延时构成绝对值/占比图。
  - `tail_probe_report.md`：可读结论与样本质量说明。

## 5. 对账原则（强制建议）

- CPU 对账：`frontend + background + kernel/wait ≈ 总 CPU`。
- I/O 对账：`前台读 + 后台读写 + 元数据开销 ≈ 设备读写`。
- 一致性检查：参数必须和 `run_registry.csv` 与 `metrics_table.csv` 一致。
- 缺测处理：若无 profile 数据，账本要明确标注“仅内部统计”。

## 6. 解释建议

- 先看 `module_impact_matrix.csv` 的同 scenario 横向对比（如 thread sweep 下同一 scenario 的变化）。
- 再看 `resource_ledger.csv` 纵向对账（一个 case 内部各项资源是否闭环）。
- 最后在 `resource_summary.md` 写结论时必须区分：
  - 观测事实（metrics）
  - 归因推断（inference）
- 正式报告主图统一使用 `phaseX_readpath_{time,bandwidth,cpu}_stage_stack.png`，不使用散点图（如 `perf_frontier_mixgraph.png`、`resource_bottleneck_scatter.png`）做主结论。
- 旧版横向占用图（`phaseA_cache_multimetric_vertical.png`、`phaseB_threads_multimetric_vertical.png`、`phaseC_mixratio_multimetric_vertical.png`）已废弃。

## 7. 已知限制（v1）

- `perf` 需要稳定 PID；矩阵模式下常仅能做粗粒度采样。
- 若 `perf_event_paranoid` 或权限限制导致 perf 采样失败，可先保留 pidstat/iostat 继续流程；遇到硬性瓶颈再升级 perf 采样。
- 未做代码级函数计时埋点（如序列化函数级 CPU）；当前为系统级+RocksDB统计级归因。
- 单次 run 抗噪有限，建议关键点做 3 次 repeat 取中位数。
