# 0. 目标与范围

## 0.1 目标

系统（或模拟器）输出的性能曲线，拟合到论文给出的 4 类行为：

1. **随机读 vs 顺序读**：Optane 的 random–sequential gap 远大于 DRAM（约 80% vs 20%）
2. **随机写 vs 顺序写**：小粒度随机写触发强烈写放大（EWR 很低）
3. **读写混合曲线**：混合负载与 NUMA/远端访问下显著劣化；远端峰值约 59%/62%（读/写）
4. **EWR 曲线**：EWR 与有效写带宽强相关（特别是 ntstore 拟合最好）

## 0.2 范围假设（先不做/后做）

* **第一阶段不强制建模 interleaving 细节**（你前面也说过先不考虑），但保留一个“可插拔模块”方便第二阶段加入 chunk/stripe 的影响（论文提到 4KB chunk/24KB stripe 是重要因素）。
* 重点建模：**XPLine=256B 介质粒度、XPBuffer~16KB 合并窗口、iMC RPQ/WPQ 排队效应、读写不对称**。  

---

## 0.3 Env FS 接口对齐说明（当前实现）

为了和 `simulated_hybrid_file_system.h/cc` 对齐，profile 执行按下面映射：

1. mode 映射（ntstore / store+clwb / store）

* 当前 Env FS 没有独立 mode 字段。
* 用 `simulate_xp_wpq_submit_ns` 近似“可见写完成延迟”：
  * `ntstore` -> `simulate_xp_wpq_submit_ns=90`
  * `store+clwb` -> `simulate_xp_wpq_submit_ns=62`
* `store` 单独拟合留在第二阶段。

2. EWR 定义映射

* 论文锚点语义：`EWR_paper = logical_write_bytes / media_write_bytes`（<=1）。
* Env FS 统计里 `ewr = media_write_bytes / logical_write_bytes` 实际是写放大倍数。
* profile 报告会同时输出 `EWR_paper` 与 `write_amplification`，锚点校验使用 `EWR_paper`。

3. local/remote 映射

* 当前 Env FS 无显式 NUMA/远端接口。
* 阶段一 profile 用参数集近似：
  * local 使用基准 `xp_latency_ns`
  * remote 按论文比例放大延迟（读 `1/0.592`，写 `1/0.617`）
* 第二阶段再引入更细的 iMC 信用/拓扑模型。

4. Fig.16 四点映射

* 论文四点不是线程 sweep 语义；Env FS 现阶段用 `threads=[1,4,8,16]` 作为四个 proxy 点对齐量级与趋势。

5. profile 执行口径

* 当前 profile 已切到 `envfs_profile_tool`（直接调用 EnvFS 的 read/write 接口），不再走 `db_bench` 的 LSM/WAL/BlockCache 栈。
* 这样可以把锚点误差主要归因到 EnvFS 模型本身，而不是上层存储引擎开销。

6. DRAM 基线与共享 XPBuffer（当前实现已补齐）

* 读延迟显式叠加 DRAM 基线：`dram_read_seq_ns=81`、`dram_read_rand_ns=101`（可配置）。
* 新增 RPQ 仲裁项：`xp_rpq_arb_ns`（可配置），按队列中已有未完成读请求数累加。
* `simulated_read_delay_ns` 现在为：`queue + xp_service + dram_base + rpq_arb`。
* XPBuffer 读写共享开关：`xp_share_buffer_between_rw=true`（默认开启）。
  - 采用 mock 的 `tag(file)+line(offset/xp_line)` 缓存集合，容量由 `xp_buffer_bytes/xp_line_bytes` 决定；
  - 读路径先查集合命中，未命中行走 XP 服务延迟，命中行走 prefetch 命中延迟；
  - 写路径在开启共享时把写入行注入同一集合，会触发淘汰并影响后续读命中；
  - 因此读写混合退化主要通过“命中率下降 -> 读 media 延迟上升”体现。

7. 2026-02-08 拟合状态（本轮）

* 新增 deterministic replay 路径：
  - profile tool 支持 `--deterministic_schedule` + `--deterministic_chunk_ops`；
  - 通过 `SetSimulatedFsThreadTagForCurrentThread()` 固定逻辑线程标签，保持 RPQ/WPQ 与 stream 状态可控。
* `run_envfs_profile.sh` 默认已切到 deterministic profile（`DETERMINISTIC_SCHEDULE=true`、`DETERMINISTIC_CHUNK_OPS=1`）。
* 当前默认参数下锚点覆盖已稳定达到 `27/27`（重复运行结果一致）。
* 关键读侧参数（默认值）：
  - `LOCAL_RPQ_PARALLELISM=16`
  - `LOCAL_RPQ_ARB_NS=100`
  - `BW_RPQ_ARB_NS=160`
  - `REMOTE_RPQ_ARB_NS=160`
* 这组参数在保持写可见延迟语义不变的前提下，补齐了：
  - Fig.16 read 带宽高线程点；
  - Fig.17 read local/remote 峰值线程与 remote-read ratio。
* 已支持按 level 精确挂接 XP 仿真（用于 RocksDB 主流程）：
  - `db_bench` 新增 `--simulate_xp_levels`（如 `0,1`）；
  - 通过 flush/compaction 事件回调维护 `sst -> level` 映射；
  - Env FS 根据该映射仅对目标 level 的 `.sst` 执行 XP 仿真。

---

# 1. 参考数据与拟合锚点（必须命中）

## 1.1 微基准延迟锚点（Fig.2）

* DRAM：seq read 81ns，rand read 101ns
* Optane：seq read 169ns，rand read 305ns（random 比 sequential 约 +80%）

> 用途：固定“读路径基础延迟”和“随机惩罚系数”。

## 1.2 宏观 I/O 带宽锚点（Fig.16, FIO）

读带宽（GB/s）：seq `[30.0,19.1,32.8,25.6]`，rand `[25.7,22.2,26.3,25.5]`
写带宽（GB/s）：seq `[8.6,7.0,9.2,8.5]`，rand `[6.7,6.0,8.2,7.5]` 

> 用途：校准“有效带宽上限”和“随机/顺序差距在系统栈下的体现”。

## 1.3 写放大/EWR 锚点（单线程、ntstore、随机写）

* 64B：EWR=0.25
* 256B：EWR=0.98 

> 用途：把“<256B 随机写断崖”落成可拟合参数。

## 1.4 XPBuffer 合并窗口锚点（Fig.9）

* 64 条 XPLine（每条 256B）≈ 16KB：窗口内 EWR 接近 1；超过后写放大显著上升 
  并且介质粒度 XPLine=256B 是前提。

## 1.5 读写混合/NUMA 锚点（Fig.17）

* 远端 Optane 峰值带宽约为本地的：读 59.2%，写 61.7%
* 最优线程数：local read 16、remote read 10、write 4 

---

# 2. 仿真总体架构（分层模型）

建议做成 **“请求级”仿真**（离散事件或时间推进均可），核心是让每个请求经历同样的路径：

**CPU侧请求生成 → iMC 队列（RPQ/WPQ）→ DIMM 控制器（XPController：AIT+调度）→ XPBuffer（合并/行缓冲）→ 介质 XPLine 服务**

论文明确 iMC 有 RPQ/WPQ。
DIMM 内抽象为 XPController/AIT/XPLine/XPBuffer。

---

# 3. 核心拟合骨架（可以直接实现的数学/规则）

下面是最小但可解释的骨架：**延迟模型 + 带宽模型 + EWR 子模型 + 并发/队列子模型 + 读写混合/远端系数**。

## 3.1 请求定义

每个请求 `req` 包含：

* `op ∈ {R,W}`
* `size`（bytes）
* `pattern ∈ {seq, rand}`（可以用 stride/地址分布推断）
* `locality_window`（用于 XPBuffer 命中/合并，单位 bytes）
* `mode ∈ {ntstore, store+clwb, store}`（写路径用；论文讨论这三种）

> 接口对齐：当前 mode 通过 profile 参数集近似，不是 Env FS 的独立字段（见 0.3）。

---

## 3.2 EWR 子模型（写的“有效效率”）

目标：输出 `EWR(size, randomness, locality_window, mode)`，作为写带宽的核心调制量。

### 3.2.1 基于 XPLine=256B 的粒度惩罚

令 `L = 256B`（XPLine）

* 若 `size >= L` 且对齐：`EWR_size ≈ 1`
* 若 `size < L`：`EWR_size` 下降，且随机性越强越差

**锚点约束（必须满足）：**

* `EWR(ntstore, rand, 64B) = 0.25`
* `EWR(ntstore, rand, 256B) = 0.98` 

> profile 校验使用 `EWR_paper = logical/media`，并同步输出 `write_amplification = media/logical`。

> 实现建议：用分段线性或幂函数拟合
> `EWR_size = a * (size/L)^p`（对 size<L），并用上面两点解出 `a,p`（或用一条分段线性穿过这两个点，最快落地）。

### 3.2.2 XPBuffer 合并窗口模型（16KB）

令 `B = 16KB`（XPBuffer 合并窗口量级）
用一个“窗口命中率”函数控制合并是否有效：

* `hit = clamp(1 - (locality_window - B)/B, 0, 1)`（只是例子）
* 合并有效时，EWR 往 1 拉；无合并时回到 `EWR_size` 或更差（RMW）

**锚点约束：**当工作集 ≤16KB 时，“EWR 近似 1”；超过后“显著上升写放大”。

最终可写成：
`EWR = hit * 1.0 + (1-hit) * EWR_size`

### 3.2.3 mode 的影响（ntstore 拟合最好）

论文表明 EWR 与 device bandwidth 在 ntstore 上拟合最好（slope 1.03, R² 0.97），其他模式相关性弱一些。
所以你可以先让 `mode` 只影响“常数系数”和“噪声/方差”，例如：

* `EWR_mode_factor(ntstore)=1.0`
* `store+clwb`、`store` 给一个 <1 的系数或更大的方差（第二阶段再精细化）

---

## 3.3 带宽模型（把 EWR 映射到有效吞吐）

把 DIMM 写路径看成“理论上限 × EWR × 并发退化项”。

### 3.3.1 单 DIMM 上限（读/写不对称）

论文给了“读带宽远高于写带宽”的强现象，例如单 DIMM 最大读/写差异（读明显更大）。
你可以先用宏观 FIO 锚点定一个“系统栈下有效上限”：

* `BW_read_cap`、`BW_write_cap` 分别拟合到 Fig.16 的 seq/rand 读写带宽量级 

### 3.3.2 写带宽

`BW_write = BW_write_cap(pattern) * EWR(size, ...) * Degrade(concurrency, queue)`

其中 `BW_write_cap(pattern)` 可以先用两个值区分：

* `BW_write_cap(seq)` 和 `BW_write_cap(rand)`（从 Fig.16 抽取）

### 3.3.3 读带宽

读没有 EWR（不涉及写放大），但需要体现顺序优势与随机惩罚（来自 XPBuffer 行缓冲解释）：

* 在 Fig.2 里，Optane random read 延迟显著更高（305ns vs 169ns）
  所以你可以用一个 `ReadLocalityBoost` 让顺序读更快：
  `BW_read = BW_read_cap(pattern) * Degrade(concurrency, queue_rpq)`

---

## 3.4 延迟模型（p50 + 长尾）

延迟分成：**服务时间 + 排队等待 + 极小概率 outlier**。

### 3.4.1 基础读延迟（锚定 Fig.2）

设：

* `Lat_dram(seq)=81ns`、`Lat_dram(rand)=101ns`
* `Lat_xp(seq)=169ns`、`Lat_xp(rand)=305ns`

当前 Env FS 实现采用可分解形式：

* `Lat_read = queue_delay + dram_base + xp_service`
* `dram_base` 来自 `dram_read_seq_ns / dram_read_rand_ns`
* `xp_service` 来自 `xp_latency_ns`、`xp_prefetch_hit_ns` 与 miss bytes

### 3.4.2 写的“可见完成延迟”

论文里写延迟（以提交到 ADR 域为完成）在 Optane/DRAM 接近：ntstore 90ns，clwb 62ns（Optane）。
你可以把写 p50 延迟先锚在这两个值（按 mode）。

### 3.4.3 排队模型（RPQ/WPQ）

iMC 侧明确有 RPQ/WPQ。
实现上用 M/M/1 或 G/G/1 近似也行，但更建议离散事件队列（FIFO + 简单调度）。

关键是给一个 **有限 inflight credit**，让高并发时出现吞吐回落/等待时间上升（你前面问的“并发>4”的现象）。论文也给了写侧最优线程数=4 的锚点（Fig.17）。

### 3.4.4 outlier（长尾）

论文观测到极少量写入会出现 ~50µs 的 outlier（热点情况下）。
建议先做一个极小概率混合分布：

* `P(outlier)=p0`（可配置，热点更大）
* outlier 延迟常数 10–50µs（先取 50µs 上界）

---

## 3.5 读写混合 & NUMA/远端访问系数

实现一个“位置系数”：

* `LocFactor_read(remote)=0.592`
* `LocFactor_write(remote)=0.617` 

并在并发曲线里强制满足“最优线程数”锚点：

* local read peak at 16, remote read peak at 10, write peak at 4 

---

# 4. 参数表（建议在文档里做成“可调旋钮”）

## 4.1 固定常量（来自论文/平台假设）

* XPLine 粒度 `L=256B` 
* XPBuffer 合并窗口 `B≈16KB` 
* read latency anchors：169ns / 305ns 
* write visible latency anchors：ntstore 90ns，clwb 62ns（Optane）
* remote factors：0.592 / 0.617 

## 4.2 待拟合参数（你系统要“靠过去”的旋钮）

* `BW_read_cap(seq/rand)`、`BW_write_cap(seq/rand)`（从 Fig.16 初始化，再按你的栈校准）
* `EWR_size` 函数的形状参数（保证过 64B=0.25、256B=0.98）
* `Degrade(concurrency)` 形状（保证读 peak 在 16/10、写 peak 在 4）
* outlier 概率 `p0`（可按热点/随机性调）

---

# 5. 拟合流程（迭代步骤与验收标准）

## Step 1：只拟合延迟锚点（无队列）

* 输出：Fig.2 四个读延迟点（DRAM/Optane 的 seq/rand 读）和 Optane 写可见延迟（ntstore/clwb）
* 验收：误差 < 5%（直接对齐锚点）

## Step 2：拟合写放大/EWR 子模型

* 让 `EWR(64B,rand,ntstore)=0.25`、`EWR(256B,rand,ntstore)=0.98` 
* 让 XPBuffer 窗口在 16KB 附近出现“合并能力断点”
* 验收：EWR 曲线拐点位置误差 < 10%

## Step 3：拟合带宽上限与随机/顺序差异（单线程/低并发）

* 用 Fig.16 的 seq/rand 读写带宽初始化上限 
* 验收：四个模式（seq/rand × read/write）的带宽落入论文点±10%

## Step 4：加入并发/队列退化（关键：峰值线程数）

* 强制满足：local read peak 16、remote read peak 10、write peak 4 
* 验收：峰值位置正确；峰值之后出现回落（非单调）

## Step 5：加入读写混合与远端系数

* 远端峰值比例对齐：读 59.2%、写 61.7% 
* 验收：远端曲线整体下移且更早受并发影响（符合论文“线程多 NUMA 更差”的结论）

## Step 6（可选）：长尾 outlier

* 验收：p99/p999 随并发增加上升；并能出现极低概率 10–50µs outlier（热点可放大）

---

# 6. 输出物与对齐方式（你拿来“把系统往曲线上靠”）

## 6.1 必做图表（你的模拟器/系统输出）

1. `Latency_p50/p99 vs size`（读 seq/rand；写 ntstore/clwb）
2. `Bandwidth vs size`（读/写 × seq/rand）
3. `Bandwidth vs concurrency`（读/写，local/remote，混合比例 0/50/100）
4. `EWR vs size`（特别标出 64B 和 256B 点；标出 16KB 拐点）

## 6.2 误差度量

* 点对点：MAE/MAPE（锚点）
* 曲线形状：峰值位置误差、拐点位置误差、单调性/非单调性一致性（写并发必须非单调）

---

# 7. 第二阶段扩展

2. **更精细的 store+clwb vs ntstore**（把 Fig.8 的 slope/R² 用到每种模式的带宽映射）
3. **WPQ/RPQ 深度与信用机制**（更贴近 iMC 行为）

---

# 8. 运行入口（Env FS）

```bash
tools/nvm_fs/run_envfs_profile.sh
```

构建依赖：

```bash
cmake -S . -B build_nvm -DCMAKE_BUILD_TYPE=Release
cmake --build build_nvm --target envfs_profile_tool -j8
```

输出：

* `analysis/envfs_profile_summary.csv`
* `analysis/envfs_profile_anchors.csv`
* `analysis/envfs_profile_report.md`

说明：

* report 直接给出 latency / EWR / bandwidth proxy / concurrency peak / remote ratio 锚点误差。
* 锚点未命中时优先调：`xp_latency_ns`、`xp_prefetch_hit_ns`、`xp_wpq_submit_ns`、`rpq/wpq depth` 和线程 sweep 区间。
