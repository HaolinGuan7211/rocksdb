# 模块宏观归因流程规范（Resource Ledger）

## 1. 适用范围
- 本规范用于回答“哪些模块在宏观上占用更多 time/bandwidth/cpu”。
- 输入来自总体性能流程，输出到 `analysis/resource_ledger/`。

## 2. 结论边界（强制）
- 本阶段是宏观归因，不直接解释单个长尾请求内部构成。
- 若需要分析 P99 长尾样本内组成，必须进入长尾探针规范。

## 3. 必备数据与字段（强制）
- `resource_ledger.csv` 必须包含性能字段与解释字段。
- 推荐强制字段：
  - `cache_miss_per_kop`
  - `iter_bytes_per_op`
  - `next_per_seek`
  - `cpu_user_pct/cpu_sys_pct/cpu_wait_pct`（有 profile 时）
  - `iostat util/await`（有 profile 时）
  - `perf ipc/cache_miss_pct`（有 profile 时）
- 必须保留原始 `rocksdb.*` 日志事件文本，用于自动抽取。

## 4. 事件抽取与归因规范（强制）
- 从日志自动发现全部 `rocksdb.*` 事件，禁止只看人工点名事件。
- 读链路阶段映射必须保序。
- `index_lookup` 与 `table_open_meta` 必须分开统计。
- 相关性筛选默认 Spearman：`|rho| >= 0.45` 且 `n >= 6`。
- 通道定义：
  - `time`：映射到 `us/op`
  - `bandwidth`：映射到 `MB/s`
  - `cpu`：仅纳入与 `cpu_total_pct` 相关事件，映射到 `cpu pct points`
- 与目标资源无关事件不得强行纳入对应通道。

## 5. 必备产物（建议强制）
- `resource_ledger.csv`
- `module_impact_matrix.csv`
- `resource_summary.md`
- `validation.json`
- `meta.json`
- `readpath_event_samples.csv`
- `event_metric_relevance.csv`
- `readpath_timeline_events.csv`
- `event_relevance_heatmap.png`
- `readpath_timeline_report.md`
- `phaseX_readpath_time_stage_stack.png`（按存在 phase 生成）
- `phaseX_readpath_bandwidth_stage_stack.png`（按存在 phase 生成）
- `phaseX_readpath_cpu_stage_stack.png`（CPU profile 可用时）

## 6. 图表规范（强制）
- 主结论图必须为阶段堆叠图，需同时体现绝对值与相对占比。
- 点状散点图不作为正式主结论图。
- phase 命名与变量口径必须一致：A->cache，B->threads，C->mix/locality。
- 旧版横向占用图已废弃，不得作为正式结论产物。

## 7. CPU 缺失补采流程（强制）
- 若 `cpu_user_pct/cpu_sys_pct` 缺失：
  - `tools/replay_profile_from_matrix_cmds.sh --matrix-dir <dir> --profile-root <dir> --cases <...> --steps 02`
  - 优先补 `mixgraph`，不默认全场景重跑。
- 重聚合必须带 profile 根目录：
  - `python3 tools/aggregate_resource_ledger.py --matrix-dir <dir> --profile-root <profile_root>`
- 补采后校验：`lane=cpu` 的分阶段绝对值之和需与 `cpu_total_pct` 对齐（允许小数误差）。
- 若 `perf_event_paranoid` 限制 perf，可先保留 pidstat/iostat，硬性瓶颈再升级 perf 采样。

## 8. 失败与异常处理（强制）
- 若 `validation.json` 有关键告警（缺表、缺列、profile 全缺失）：
  - 必须在 `experiment_result.md` 写明接受或重跑策略。
