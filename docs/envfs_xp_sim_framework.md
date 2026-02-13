# Env FS XP 仿真性能框架

这个框架用于评估 RocksDB 的 Env/FileSystem 仿真层是否能近似 PMEM 设备特征，重点覆盖：

- `latency`：端到端（db_bench）与模型注入延迟（simulated delay）
- `bandwidth`：端到端吞吐与模型介质带宽
- `EWR`（Endurance Write Ratio）：`media_write_bytes / logical_write_bytes`

## 1. 模型参数

在 `db_bench` 中新增：

- `--simulate_xp_nvm=1`
- `--simulate_xp_line_bytes=256`
- `--simulate_xp_buffer_bytes=16384`
- `--simulate_xp_latency_ns=300`
- `--simulate_xp_rpq_depth=64`
- `--simulate_xp_wpq_depth=64`
- `--simulate_xp_wpq_submit_ns=100`
- `--simulate_xp_prefetch_hit_ns=120`
- `--simulate_xp_enable_prefetch=true`
- `--simulate_xp_path_prefix=<prefix>`（可选）
- `--simulate_xp_stats_file=<path>`（输出模型统计）

仿真规则：

- 读路径：进入 `RPQ`，包含排队延迟 + 服务延迟；顺序流可命中 `XPBuffer` 预读窗口（`xp_prefetch_hit_ns`）
- 写路径：进入 `WPQ` 即提交成功（延迟约 `xp_wpq_submit_ns`）；`WPQ` 满时需要排队等待
- 调度：对同文件、相邻/重叠写请求做贪心合并；合并窗口上限为 `xp_buffer_bytes`
- 介质服务延迟：`ceil(bytes / xp_line_bytes) * xp_latency_ns`

## 2. 一键基准

脚本：`tools/nvm_fs/run_envfs_xp_bench.sh`

默认会跑两套：

- `sim`：开启 XP 仿真
- `base`：关闭仿真（基线）

命令：

```bash
tools/nvm_fs/run_envfs_xp_bench.sh
```

常用参数：

```bash
MODES=sim,base \
XP_LINE_BYTES=256 \
XP_BUFFER_BYTES=16384 \
XP_LATENCY_NS=300 \
XP_RPQ_DEPTH=64 \
XP_WPQ_DEPTH=64 \
XP_WPQ_SUBMIT_NS=100 \
XP_PREFETCH_HIT_NS=120 \
XP_ENABLE_PREFETCH=true \
PMEM_REFERENCE_CSV=/path/to/reference.csv \
tools/nvm_fs/run_envfs_xp_bench.sh
```

## 3. 输出结果

输出目录（默认）：`experiment/envfs_xp_<timestamp>/`

关键文件：

- `case_manifest.csv`：测试清单
- `stats/*.kv`：每个 case 的模型统计
- `analysis/envfs_xp_summary.csv`：汇总指标
- `analysis/envfs_xp_report.md`：结论报告

核心统计现在包含：

- 总延迟：`simulated_{read,write}_delay_ns`
- 排队延迟：`simulated_{read,write}_queue_delay_ns`
- 介质延迟：`simulated_{read,write}_media_delay_ns`
- 预读命中：`read_prefetch_hits` / `prefetch_hit_ratio`

## 4. 参考值校验

可提供 `PMEM_REFERENCE_CSV`，格式：

```csv
case,metric,target,tolerance_pct
sim_03_read_lat_256b,sim_read_latency_ns_per_op,900,20
sim_04_read_bw_256b,sim_read_bw_mib_s,6000,25
sim_02_write_bw_4k,sim_write_bw_mib_s,2500,25
sim_07_ewr_stress_64b,ewr,1.5,20
```

说明：

- `case` 必须与 `summary` 里的 case 匹配（包含 mode 前缀）
- `metric` 可选列名见 `envfs_xp_summary.csv`
- `tolerance_pct` 是允许误差百分比

报告会输出每项 `pass/fail`。

## 5. 建议验收门槛

在投入正式实验前，建议至少满足：

- 关键读写场景延迟误差在目标阈值内
- 关键读写带宽误差在目标阈值内
- `EWR` 趋势与真实 PMEM 一致（尤其小写入场景）
- `sim` 相对 `base` 的变化方向和真实设备一致
