# 总体性能分析流程规范（A/B/C）

## 1. 适用范围
- 本规范用于回答宏观问题：吞吐、时延、命中率、基础 I/O 行为。
- 对应 phase：
  - A：`cache_sweep`
  - B：`thread_sweep`
  - C：`locality_sweep`

## 2. 目标与结论边界（强制）
- 目标是回答“尾巴有多长、总体性能如何变化”。
- 本阶段不直接回答“长尾样本内部构成”与“模块排他耗时构成”。
- 模块归因和长尾拆解必须分别交给后续两份规范。

## 3. 设计约束（强制）
- 同一实验内只改目标变量，其余参数固定。
- 必须记录并锁定：数据规模、key/value、cache、threads、mix/locality、direct I/O。
- case 之间至少重开 DB，避免状态污染。

## 4. phase 专属约束（强制）
- A 类（cache）：
  - `1GiB <= cache_size <= DB_size/2`。
  - 热区工作集必须大于最大 cache 档位，避免全档位全命中。
  - `experiment_plan.md` 必须写明 `cache/DB` 与 `hot_working_set/cache` 比例。
- B 类（threads）：
  - 仅允许线程数变化；cache、mix、数据规模固定。
  - 若吞吐上升但 tail latency 或 iowait/context-switch 恶化，必须标注扩展性拐点。
- C 类（mix/locality）：
  - 仅允许 mix/locality 参数变化；其余固定。
  - 结论必须同时解释性能变化与命中率变化。

## 5. 必采集指标（强制）
- 性能：`ops_per_sec`、`throughput_mb_s`、`micros/op`、`seek p50/p95/p99/p100`。
- cache/I/O：`block cache hit/miss`、`cache_hit_ratio_pct`、读 bytes/reads。
- 上下文：`L0 files`、`cumulative writes`、`uptime`。
- 解释字段：`cache_size`、`threads`、`read mode`、`locality`、`direct I/O`。

## 6. 必备运行与监控产物（强制）
- `matrix_runner.log`
- `monitor_status.json`
- `monitor_events.log`
- `monitor_summary.txt`
- `run_registry.csv`

## 7. 报告与图表要求（强制）
- `experiment_result.md` 必须覆盖：实测配置、核心图、预期对比、异常、置信度。
- `matrix_dashboard` 只展示本次实际执行的 phase，不混入未执行面板。
- heatmap 文字颜色必须自适应背景亮度。
- 本阶段主图应体现绝对值与趋势；不要用单点做趋势结论。

## 8. 有效性判定（强制）
- 以下任一成立即判无效并重跑：
  - 计划与实测参数不一致。
  - 命中行为明显异常（如极小 cache 仍近似全命中）。
  - 指标缺失到无法支撑结论。
  - 仅有单点却被用于趋势结论。

## 9. 与后续流程衔接
- 输出作为模块宏观归因输入：`metrics_table.csv`、运行日志、case 索引。
- 输出作为长尾探针输入：稳定场景的 `seek p99` 基线与 case 选择（默认 A2/B2/C2）。
