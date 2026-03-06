# RocksDB Benchmark 标准化实验工作流总纲（v2）

## 1. 目标与分层
- 本文档是总纲，只定义全局强制规范与流程边界。
- 具体流程拆分为 4 份专项规范，统一放在 `experiment/workflow_specs/`：
  - `01_overall_performance_workflow.md`：总体性能分析流程（宏观性能层）。
  - `02_module_macro_analysis_workflow.md`：模块级宏观归因流程（资源账本层）。
  - `03_p99_tail_probe_workflow.md`：P99 长尾探针流程（尾部样本层）。
  - `04_zigzag_staging_mvp_workflow.md`：ZigZag staging MVP 对照流程（功能与收益验证层）。
- 三层关系固定：先总体性能，再模块宏观，再 P99 长尾。禁止跳层直接下细粒度结论。

## 2. 命名与目录规范（强制）
- 所有正式实验放在 `./experiment/`。
- 目录命名：`<YYYYMMDD>_experiment<GLOBAL_ID>_<topic>`。
- `GLOBAL_ID` 全局唯一；A1/A2/A3 只是同一 experiment 的 case。
- 统一归档结构：
  - `analysis/`：跨 case 汇总图与总报告。
  - `analysis/resource_ledger/`：资源账本与模块分析产物。
  - `analysis/resource_ledger/tail_probe/`：长尾探针产物。
  - `cases/<case_id>/`：单 case 计划、结果、运行日志。
  - `run_registry.csv`：case 台账。
  - `matrix_index.md`：实验索引。

## 3. 全局必备文档（强制）
- `experiment_plan.md`（实验前）：
  - 目标、预期、变量/固定项、数据规模、key/value、locality、cache 档位、关键开关、风险与成功判据。
- `experiment_result.md`（实验后）：
  - 实测配置、核心结果、预期对比、异常解释、置信说明、下一步动作。

## 4. 全局运行约束（强制）
- 使用 Release 构建。
- 同一实验内只改变目标变量，其他参数固定。
- case 间必须做干扰控制（至少重开 DB）。
- 建议启用 direct I/O，避免 OS page cache 污染。
- 清理时只删已声明目录，禁止模糊删除。

## 5. 全局有效性与图表规范（强制）
- 若出现参数不一致、命中退化、图表无法支撑趋势、关键指标缺失，结果标记无效并重跑。
- 报告正文优先中文（术语/路径/指标名除外）。
- phase 与变量口径必须严格对应：A->cache，B->threads，C->mix/locality。
- 资源账本正式结论图使用阶段堆叠图；点状散点图不得作为主结论图。
- 废弃图禁止进入正式结论：`phaseA_cache_multimetric_vertical.png`、`phaseB_threads_multimetric_vertical.png`、`phaseC_mixratio_multimetric_vertical.png`。

## 6. 三份专项规范入口
- 总体性能流程：`experiment/workflow_specs/01_overall_performance_workflow.md`
- 模块宏观归因流程：`experiment/workflow_specs/02_module_macro_analysis_workflow.md`
- P99 长尾探针流程：`experiment/workflow_specs/03_p99_tail_probe_workflow.md`
- ZigZag staging MVP 流程：`experiment/workflow_specs/04_zigzag_staging_mvp_workflow.md`

## 7. 快速执行顺序
1. 按总体性能流程完成 A/B/C 变量矩阵与基础结论。
2. 按模块宏观归因流程产出读路径 time/bandwidth/cpu 归因。
3. 按 P99 长尾探针流程做阈值采样与分桶构成分析。
4. 回填 `experiment_result.md`，完成有效性审阅与归档。
