# P99 长尾探针流程规范（Tail Probe）

## 1. 适用范围
- 本规范用于回答“长尾请求的延时构成是什么、不同长尾等级构成是否变化”。
- 长尾探针是 A/B/C phase 的内嵌流程，不是独立第四类实验。

## 2. 核心原则（强制）
- 宏观账本用于回答“尾巴有多长”；尾部样本构成由本流程回答。
- 优先分析延时构成，再决定是否深入 CPU/I/O wait/带宽。
- 阈值必须基于重复基线估计，禁止单次 p99 直接下结论。

## 3. 阈值与采样规则（强制）
- 每个 phase 至少选一个代表 case（默认优先 A2/B2/C2）。
- 在相同负载下重复运行 `mixgraph`，估计稳态 p99 分布。
- 长尾阈值：
  - `threshold_us = max(min_threshold_us, median_p99_us * multiplier)`
- 仅采样 `latency_us >= threshold_us` 的 seek 请求。
- 必须记录：重复次数、mean/std/CI95/CV、阈值、样本数。

## 4. 计时口径（强制）
- stage 必须使用排他计时口径（父子重叠去重 + 单请求封顶）。
- 同一请求的 stage 分解用于构成分析时，必须可归一到 100%。

## 5. 长尾分桶规则（强制）
- 对每个 case 按 `latency_us / threshold_us` 分桶：
  - `1.0-1.2x`
  - `1.2-1.5x`
  - `1.5-2.0x`
  - `2.0-3.0x`
  - `3.0-5.0x`
  - `>=5.0x`
- 每个分桶都要给出组件构成与样本数，比较不同长尾等级是否出现构成漂移。

## 6. 必备产物（强制）
- `tail_probe/tail_threshold_summary.csv`
- `tail_probe/tail_stage_samples.csv`
- `tail_probe/tail_stage_breakdown.csv`
- `tail_probe/tail_stage_distribution.csv`
- `tail_probe/tail_latency_bucket_breakdown.csv`
- `tail_probe/phaseX_tail_seek_stage_stack.png`（按存在 phase 生成）
- `tail_probe/phaseX_tail_component_share_distribution.png`（按存在 phase 生成）
- `tail_probe/phaseX_tail_latency_bucket_stage_stack.png`（按存在 phase 生成）
- `tail_probe/tail_probe_report.md`

## 7. 推荐执行入口
- 全流程入口：
  - `tools/run_standard_matrix_50gb.sh`（开启 `ENABLE_TAIL_PROBE=1`）
- 手动入口：
  - `python3 tools/run_tail_probe_from_matrix.py --matrix-dir <dir>`
- 仅重算图表/统计（复用已有样本）：
  - `python3 tools/run_tail_probe_from_matrix.py --matrix-dir <dir> --reuse-existing-samples`

## 8. 解释规范（强制）
- 先给分桶样本规模，再给构成变化结论。
- 必须区分：
  - 条件占比 `P(lat>=k*threshold | lat>=threshold)`
  - 全量请求占比（需结合总体请求量估计）
- 若启用 `max_samples` 截断，报告必须声明“尾部样本比例存在采样偏差风险”。
