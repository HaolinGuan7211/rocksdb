# KV-SEP + B+Tree SST 方案（实验设计书）

目标：在 `simulated_hybrid_file_system`（simfs）仿真环境中，基于 RocksDB 的 block-based table（SST），实现一套**可开关、可回退**的“B+Tree 索引 + Key/Value 分离”SST 变体，并用 mixgraph（保留 shift + burst）做对比压测，产出 P50/P95/P99/P99.9、Tail Probe、burst/shift impact、simfs latency 等图像与 CSV。

> 本文档对应分支：`feature/bptree_kvsep_sst`

---

## 1. 背景与动机

在 cache=0 的 simfs 场景中，Seek 的尾延迟往往与“每次 Seek 触发的底层读次数（block read count）”强相关。即便 NVM 模拟介质单次 IO 延迟很低，**IO 次数**仍可能主导 tail。

而在“真实线上”（cache=500MB 级别）的场景里，Block Cache 会兜住大量热数据，tail 往往来自：

- 少量穿透到 simfs 的读（包括 value/data block）
- burst/scan 流量对 cache 与底层读路径的扰动
- seek/iterator 触发的额外读放大（例如跨多个 SST/level）

因此我们希望探索一个“论文可写”的 SST 结构改造点：**修改 SST 的 index 与 data block 组织方式**，目标是在满足 mixgraph（Seek + MultiGet + Scan + shift + burst）前提下，降低 seek/read 的 IO 读放大，并解释 P99 的构成变化。

---

## 2. 方案概述（你提出的约束逐条对应）

你给出的目标约束：

1) **实现一套 B+Tree 索引的 SST**
2) **SST data block Key/Value 分离**
3) **key 和 index block 放在一起**
4) **value 单独构成 data block**
5) **SST index block 存储 key + data block offset**

本分支计划实现的“KV-SEP + B+Tree SST”结构：

- **Value Data Blocks（值块）**：只存 value（不存 key），按 block 切分与压缩/校验；支持按“块内 offset”取回 value（避免再做 key compare）。
- **Leaf Blocks（叶子块）**：存储有序的 **key + value pointer**。value pointer 至少包含：
  - value block handle（offset/size）
  - value 在 block 内的 offset（以及必要时 value 长度）
- **Internal Index Blocks（内部索引块）**：存储 separator key + child block handle，形成 B+Tree。
- **Root/Meta**：root handle 写入 meta 或 properties，打开 SST 时快速定位。

这样：

- “key + index block 放在一起” → key 全部在 leaf/internal blocks 中
- “value 单独构成 data block” → value blocks 只存 value payload
- “index block 存储 key + data block offset” → leaf 里存 key + value block offset（以及块内 offset）

---

## 3. On-disk 格式草案（版本化 + 可回退）

### 3.1 开关与兼容性策略

- 默认不开启 KV-SEP/B+Tree：完全使用 RocksDB 原版 SST 格式与读写路径。
- 开启后：写出的 SST 在 table properties 里记录：
  - `rocksdb.experimental.kvsep = 1`
  - `rocksdb.experimental.kvsep.format_version = 1`
  - `rocksdb.experimental.kvsep.bptree.root_handle = <encoded>`
  - （可选）统计信息：leaf 数、value block 数、平均 key/value bytes 等

读路径：

- 检测到上述 properties → 使用 KV-SEP/B+Tree reader + iterator
- 否则 → 走原版 reader

回退：

- 关闭开关并重新 fill DB（清库）即可回到原版 SST。

### 3.2 Value Data Block（只存 value）

为了让 value 取回可以 O(1) 定位，值块建议采用“顺序 values + offsets 数组”的结构：

```
VALUE_BLOCK :=
  [ VALUE_BYTES ... ]
  [ OFFSETS(uint32_t) ... ]   // 每条 value 的起始 offset（相对 block 起点）
  [ NUM_VALUES(uint32_t) ]
  [ FOOTER(uint32_t) ]        // magic/flags（用于快速校验/识别）
```

写入时：append value bytes，并记录 offset；block 满则 flush。

读取时：读到整个块后，通过 offsets + value length 推导出 slice（value_length 可以由 offsets[i+1]-offsets[i] 得到，最后一个通过 block_end 推导）。

### 3.3 Leaf Block（key + value pointer）

Leaf block 可以复用 RocksDB 的 BlockBuilder（prefix-compress + restarts），存储：

- key: user key（或 internal key，取决于 iterator 语义）
- value: `ValuePtr` 编码

`ValuePtr` 编码建议：

- `ValueBlockHandle`（RocksDB BlockHandle 的 varint 编码：offset + size）
- `value_offset_in_block`（fixed32 或 varint32）
- `value_length`（varint32，若能从 offsets 推导也可省略）

### 3.4 Internal Index Block（separator key + child handle）

同样可以复用 BlockBuilder：

- key: separator key（B+Tree 内部节点 key）
- value: child block handle（指向下一层 internal 或 leaf block）

---

## 4. 读写路径（实现范围）

### 4.1 写路径（TableBuilder）

新增一套 experimental builder：

- `KVSepValueBlockBuilder`：构建 value blocks（只存 value）
- `KVSepLeafBuilder`：构建 leaf blocks（key + value ptr）
- `KVSepBptreeBuilder`：把 leaf blocks 组织成 B+Tree（多层 internal blocks）

写 SST 流程：

1) 每条 KV：
   - value 写入当前 value block buffer，得到 `(current_value_block_handle_placeholder, in_block_offset)`
   - key + value_ptr 写入 leaf builder
2) value block 满 → flush value block，获得真实 block handle
3) leaf block 满（或达到 leaf target）→ flush leaf，记录 leaf handle + leaf first key
4) Finish：
   - 根据 leaf 列表构建上层 internal blocks，直到 root
   - root handle 写入 properties/meta

### 4.2 读路径（TableReader + Iterator）

核心能力要求（对 mixgraph）：

- `Seek`：必须能定位到存在的 key（避免频繁打空）
- `MultiGet`：必须命中 DB 内 key
- `Scan/Iterator Next`：需要支持持续 next（用于 burst scan 流量）

读路径设计：

- `BptreeIndexReader`：从 root 下降到 leaf，leaf 内二分定位 key，得到 value_ptr
- `KVSepTableIterator`：
  - 维护当前 leaf iterator（key 顺序）
  - `Next()`：leaf 内 next；必要时切换到下一个 leaf（通过 sibling 或上层索引定位）
  - `value()`：按 value_ptr 从 value block 取回 value slice

关于 value block 的 IO 与 cache：

第一阶段（POC）建议：

- cache=0：直接读 value block 并 slice（避免引入复杂的 cache entry 类型）

第二阶段（加强）再做：

- 把 value blocks 作为 block cache 的独立 role，支持 cache=500MB 的公平对比

---

## 5. db_bench 开关与参数（便于实验矩阵）

新增 db_bench flags（建议）：

- `--experimental_kvsep_enable=1`
- `--experimental_kvsep_bptree_fanout=<N>`（例如 64/128）
- `--experimental_kvsep_leaf_block_bytes=<bytes>`（例如 16KB/32KB）
- `--experimental_kvsep_value_block_bytes=<bytes>`（例如 16KB/32KB）
- `--experimental_kvsep_store_value_len=0/1`

并确保：

- 关闭这些 flags → 不影响现有格式
- 开启这些 flags → 必须清库 fill 一次（SST 格式改变）

---

## 6. 实验方案（对比矩阵 + 指标 + 产物）

### 6.1 环境与负载（沿用“最近设定”）

- `simulated_hybrid_file_system` 打开
- `redirect_to_tmpfs=1`，保证“纯 simfs 环境”避免 base FS 噪音
- mixgraph 参数沿用最近 tuned：
  - shift：`mix_shift_stage_seconds=30`、`step_jump` 等
  - burst：`mix_burst_interval_ops=50000`、`mix_burst_scan_nexts=200`、`cold_ranges_only=1`
  - mix ratio：`Seek` 为主，包含 `MultiGet`、少量 `Get`，保留 scan burst

### 6.2 DB 构造

- 单 DB：约 4GiB（key=16B, value≈1024B, lz4）
- 每个格式变更都遵循：`rm -rf DB_DIR WAL_DIR TMPFS_ROOT` → `fillseq` → `mixgraph`

### 6.3 对比矩阵（第一轮）

目标：只对比“原版 SST vs KV-SEP+B+Tree SST”，先在 cache=0 场景把链路跑通并观察 IO 放大趋势。

- Case A（baseline）：原版 block-based table
- Case B（kvsep+bptree）：开启 `experimental_kvsep_enable=1`

分别跑：

- cache=0
- cache=500MB（第二轮，在实现 value cache 支持后跑）

### 6.4 指标

必须产出：

- Throughput：ops/sec、MB/s
- Latency：Seek/Read 的 P50/P95/P99/P99.9
- PerfContext（尤其）：
  - `block_read_count`
  - `block_read_byte`
  - `seek_child_seek_count`
- Tail Probe（P99 组成）：
  - read/open/prefetch 相关链路
- burst/shift impact：
  - `burst_vs_cache_miss`（或 burst vs 读放大指标）
  - `shift stage summary`
- simfs：
  - `sim_max_latency_per_window`（可选 read/open/prefetch 维度）

### 6.5 产物（图像 + CSV）

- `mixgraph_run_metrics.csv`（按 run_dir 汇总）
- case compare 图：
  - throughput 对比
  - seek 分位数对比
- monitoring 图：
  - simfs latency timeseries
  - burst vs miss/lat
  - stage_summary（shift 影响）
- Tail Probe 图：
  - P99 样本分布/构成（stack 或占比）

---

## 7. 风险点与验收门槛

### 风险点

- KV-SEP 改动会触及 TableBuilder/Reader/Iterator 的核心链路，容易出现：
  - Seek 打空（key->value_ptr 映射错误）
  - Iterator Next/Prev 语义不一致
  - 读放大反而增大（leaf 太大、value block 读多）
- cache=500MB 下如果 value blocks 不进 block cache，会导致不公平对比

### 第一轮验收门槛（cache=0）

- Seek/MultiGet 命中率接近 100%（负载本身 key 分布不变）
- 不出现明显的 seek 打空或 iterator corruption
- 至少在某个指标上看到可解释的方向性变化：
  - `block_read_count/seek` 下降，且 P99 跟随改善；或
  - P99 构成中某类耗时占比显著变化（Tail Probe）

---

## 8. 下一步（本分支的实施顺序建议）

1) 先实现最小可跑通版本（cache=0）：
   - 仅支持 mixgraph 需要的 Seek/Next/MultiGet
2) 再补 value blocks 的 cache 支持（cache=500MB 公平对比）
3) 再扩展 B+Tree 深度、fanout tuning、leaf/value block size sweep

