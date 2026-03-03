# SimFS “CPU dominance” calibration (mixgraph-only)

本目录的目标：把 `simulated_hybrid_file_system`（SimFS）的 **介质服务时间/带宽** 调到一个更接近 NVM 的区间，使得 RocksDB/LSM 的传统 CPU 因素（Bloom/Ribbon filter、compression 等）在 **A/B** 中变得“显眼”（>=~1.2x 的吞吐或 tail 指标差异），而不是始终被 I/O 等待主导。

硬性约束对齐：
- 只使用 `experiment/EXPERIMENT_WORKFLOW.md` 体系下的自研 workload：**mixgraph**。
- 不引入 YCSB 或“拿 db_bench 自带 workload 当替代 bench”。本实验只通过 repo 内的 runner 脚本驱动 mixgraph。
- 最小必要的 sweep + A/B；每个点位 **3 次取中位数**；原始日志与汇总 JSON 均落盘到 `results/`。

---

## 1. Bench 选择与运行方式（来自 workflow/关联文档）

本实验使用 shortscan framework 的 runner：
- runner：`tools/run_shortscan_compare.sh`
- 关联说明：`docs/shortscan_framework.md`

该 runner 会做一次建库（fill）后运行 `mixgraph` step，并在 `OUT_DIR/figures/metrics_table.csv` 写出可稳定提取的指标（吞吐/尾延迟等）。

### 1.1 READ 场景（READ_HEAVY）
选择：`mixgraph`，并通过 runner 的环境变量设为 **seek/point-lookup 主导、短扫描**，用于放大 filter/search CPU：
- `MIX_GET_RATIO=0.10`
- `MIX_PUT_RATIO=0.00`（只读）
- `MIX_SEEK_RATIO=0.90`
- `--mix_max_scan_len=1`（短扫描，避免长 scan 把顺序带宽效应混进来）

实现上这些都是 `tools/run_shortscan_compare.sh` 已暴露的 mixgraph 参数（可复现、无需改代码/手工改文件），并在每次 run 的 `config.txt`/`*.cmd` 中回显。

### 1.2 WRITE 场景（WRITE_HEAVY，用于带宽 sanity）
选择：仍然是 `mixgraph`，但改为写占比更高，用来触发 flush/compaction 背景读写，从而估计顺序/后台带宽数量级：
- `MIX_GET_RATIO=0.10`
- `MIX_PUT_RATIO=0.60`
- `MIX_SEEK_RATIO=0.30`

备注：校准的核心拐点判定主要依赖 READ_HEAVY 下 BF/Compression 的 A/B 敏感性；WRITE_HEAVY 主要用于 D 部分的 “bandwidth sanity”。

---

## 2. SimFS 可校准 knob 与语义（必须来自实现）

本次校准使用 SimFS 的 **Single-DIMM NVM 模型**（在 `db_bench` 中打开）：
- 开关：`--simulate_dimm_nvm=1`

另外，为了更贴近论文里“XPBuffer 是客观存在、但 4KB 请求不应简单等价为 16 次 256B 串行读”的语境，本仓库也支持 **XPBuffer + DIMM device** 的混合模式：
- 开关：`--simulate_xp_nvm=1 --simulate_xp_use_dimm_device_model=1`
- 含义：
  - XPBuffer（prefetch/hit/miss 计数、`xp_line_bytes`/`xp_buffer_bytes` 等）仍按 XP 模型逻辑生效；
  - 但对 miss 的 device service time 改为更像 DIMM：`fixed_overhead + request_round_trip(xp_latency_ns) + transfer(bytes/bw)`，避免把 4KB miss 直接当作 `16 * 300ns` 串行惩罚。

校准 knob（服务时间相关）：
- `--simulate_dimm_fixed_read_overhead_ns=<N>`

语义（实现路径）：
- 该参数在 `tools/nvm_fs/simulated_hybrid_file_system.cc` 的 `SimulateDimmReadQueueServe()` 中作为 **每次 Read 请求的固定 host-side 开销**，在进入 read queue/transfer 之前先执行：
  - `AdvanceSimNowNs(... begin + fixed_overhead_ns)`
  - 然后 `SleepForNanoseconds(fixed_overhead_ns)`（>=1000ns 用 `SleepForMicroseconds`，<1000ns busy-spin）
- 读写不对称：写有独立的 `--simulate_dimm_fixed_write_overhead_ns`（本实验保持为 0）。
- 该模型还有队列深度与带宽项，读路径还会叠加：
  - 队列排队等待（受 `--simulate_xp_rpq_depth` 等影响）
  - 传输时间（由 `media_bytes / bandwidth` 得到）

### 2.1 从 knob 推导 “随机读服务时间”的近似
为了把 “介质随机读服务时间” 扫到 BF-check 的同量级，本实验使用下述近似（足够用于 sweep 生成）：

`T_media_random_read(bytes) ~= fixed_read_overhead_ns + bytes / (seq_read_bw_gbps * rand_bw_scale)`

其中 `seq_read_bw_gbps` 在实现里按 **decimal GB/s** 解释（等价于 **bytes/ns**），见 `GetDimmTransferTimeNs()` 的注释。

为避免“带宽项”抢占 sweep 目标，本实验会自动把 `simulate_dimm_seq_read_bw_gbps` 设到足够大，使得在最小目标点位下传输时间 <= 目标的 25%（详见 `tools/calibrate_cpu_dominance.py`）。

---

## 3. 标尺：测量 BF-check 的纯 CPU 成本 T_filter_check

校准的核心是“比值型标定”：
- 先测出 BF check 的中位数 CPU 成本 `T_filter_check`
- 再生成介质随机读服务时间 sweep 点位：
  - `T_media_random_read ≈ {2x, 1x, 0.5x} * T_filter_check`

### 3.1 实现方式（最小侵入）
为了稳定得到 BF-check 的 CPU 时间，本仓库新增了一个 PerfContext 计时字段并在 filter 热路径上加了 `PERF_TIMER_GUARD`：
- `include/rocksdb/perf_context.h`：新增 `bloom_filter_maymatch_nanos`
- `monitoring/perf_context.cc`：将该字段加入可输出的 PerfContext metrics
- `table/block_based/full_filter_block.cc`：对 `filter_bits_reader->MayMatch(entry)` 包一层计时

runner 在 “测量标尺” 阶段把 `--perf_level` 调到较高等级，并在 mixgraph 日志里解析：

`T_filter_check_ns = bloom_filter_maymatch_nanos / (bloom_sst_hit_count + bloom_sst_miss_count)`

3 次重复取中位数作为标尺。

---

## 4. A/B 旋钮与通过条件

每个 sweep 点位（2x / 1x / 0.5x）下，在 READ_HEAVY 场景跑如下 A/B（每点每配置 3 次中位数）：

1) **Bloom filter**：ON vs OFF
- ON：`--bloom_bits=128`（可通过 `--bloom-bits-on` 改）
- OFF：`--bloom_bits=0`

2) **Compression**：ON vs OFF（主要验证 CPU 开销）
- ON：`--compression_type=zstd`（默认）
- OFF：`--compression_type=none`
- 同时固定 `--compression_ratio=1.0`，使 value 近似不可压缩，避免 I/O 节省掩盖 CPU 成本。

判定逻辑（写死在 `tools/calibrate_cpu_dominance.py`）：
- 在某个点位及以下（介质更快）满足：
  - `max(throughput_ratio, tail_ratio) >= 1.2`（对 BF 或 Compression 任一项成立）
- 且介质越快（2x→1x→0.5x）影响趋势 **不减弱**（允许少量噪声 eps）。

---

## 5. 带宽 sanity（WRITE_HEAVY）

在最快的 sweep 点位（0.5x）下，对 WRITE_HEAVY 场景读取 SimFS stats：
- `media_{read,write}_bytes`
- `simulated_{read,write}_media_delay_ns`

估计有效带宽数量级（decimal GB/s）：

`bw_read_gbps ≈ media_read_bytes / simulated_read_media_delay_ns`

该值用于避免“顺序/后台读写带宽被模型压得过低”，使得校准结果更接近论文语境的 NVM 带宽数量级（不要求精确复刻某个固定数）。

---

## 6. 一键运行与产物

一键入口：
```bash
bash tools/run_calibrate_cpu_dominance.sh
```

产物：
- 汇总：`results/calibration.json`
- 原始 run：`results/calibration_runs/<timestamp>/...`
  - 每个点位/配置/重复都有独立目录
  - runner 的 `*.log`、`*.cmd`、`config.txt`、`figures/metrics_table.csv`
  - `simfs_stats/*.simfs_stats.txt`（由 runner 注入 `--simulate_xp_stats_file` 落盘）

---

## 7. 本次实测摘要（2026-03-03）

本次 `results/calibration.json`（对应 runs root：`results/calibration_runs/20260303_020738/`）的关键数字：

- `T_filter_check`（BF-check CPU 标尺）：`~272.36 ns/check`（3 次中位数）
- sweep 点位（随机读 4KiB）自动生成的 `simulate_dimm_fixed_read_overhead_ns`：
  - `2x`: `510 ns`
  - `1x`: `238 ns`
  - `0.5x`: `102 ns`
- A/B（READ_HEAVY）：
  - BF：`throughput_ratio` 约 `2.21x~2.25x`，`tail_ratio` 约 `3.18x~3.25x`（满足 >=1.2 且随介质更快不减弱）
  - Compression：本次观测到 `COMP_OFF/BASE` 的吞吐比约 `0.25x`（说明 compression 对该负载/数据形态影响很大；详见 `results/calibration.json` 的 per-point 明细）
- 带宽 sanity（WRITE_HEAVY, 0.5x 点位）：
  - 估计 `media_read` 带宽中位数：`~122.63 GB/s`
  - 估计 `media_write` 带宽中位数：`~37.99 GB/s`
  - 说明：该数值主要反映本次校准中设置的 dimm 带宽上限与统计口径（`media_bytes / media_delay_ns`）；至少表明模型没有把顺序/后台带宽压到“像慢盘”的量级。
