- 主负载按 FAST’20/UDB 的短 scan 结构约束来：>60% scan length=1、有少量中等 scan、极少长尾、并体现 200 台阶（range limit）。
- 同时把 key-space locality做出来（热点集中在少数 key-range），因为论文强调 locality 不对会让 I/O（cache hit/read-bytes）严重失真。
- 在“不采集 trace、也不大改代码”的约束 B 下：mixgraph 做“真实感主负载”，再用 seekrandom + seek_nexts=200 轻量注入“200 台阶”。（seekrandom 的定义就是：随机 seek 后 Next seek_nexts 次）。(GitHub)
- mixgraph 的关键参数（iter/value/key/keyrange/QPS + get/put/seek 比例）在 RocksDB wiki 里有明确说明与示例。(GitHub)

---
1. 两套负载
A. 主负载（Realistic / FAST’20 风格）
用途：评估你“短 Scan 优化的 KV 文件格式”在更接近线上结构下的真实收益。
- 核心形状：短 scan 主导（>60% = 1）+ 少量中等 scan + 极少长尾 + 200 台阶注入
- locality：热点集中到少数 key-range
实现方式（无代码改动）：
- 用 mixgraph 合成 (Get/Put/Seek+Next)数（locality）。(GitHub)
- 另00` 注入“200 台阶”。(GitHub)
B. 最坏对照负载（Worst locality / YCSB-style）
用途：证明格式优化不是只在“理想 locality”下有效；量化最坏情况下退化边界。论文指出 locality 不对会导致底层 I/O 行为不一样。
实现方式（仍无改动）：
- 用一组 seekrandom 分段/循环运行，把热点“打碎”成全局随机 key-space（seekrandom 本身就是随机 seek），并用多个 seek_nexts 混合出“短为主 + 台阶 + 长尾”的 scan 长度结构。

---
2. 数据规模与资源分布
由于有 2TB SSD，不缺空间。建议用两档：
- S 档（快速迭代）：NUM_KEYS = 50,000,000（50M）
  - 适合频繁调参/调格式
- M 档（论000`（150M）
  - 更容易把 cache/I/O 差异拉大，图更“科研”
统一建议：
- key_size = 16（常见且省空间）
- value_size = 256 或 512（你可以按论文/你的系统实际再调；这不影响“短 scan 结构”本质）
cache 三档（用于画曲线）：
- 4GB / 8GB / 16GB

---
3. 目录与通用参数（先设变量，所有命令可复用）
export ROCKSDB_HOME=~/rocksdb        # 你的 rocksdb 源码目录
export DB_BENCH=$ROCKSDB_HOME/db_bench
export DB_DIR=/mnt/ssd/rocksdb_shortscan_db
export WAL_DIR=/mnt/ssd/rocksdb_shortscan_wal

mkdir -p $DB_DIR $WAL_DIR
建议加的通用项（减少噪声）
- 固定输出统计：--statistics + --benchmarks="...,stats" (GitHub)
- 直读绕过 OS cache（可选，但推荐）：-use_direct_reads=true、-use_direct_io_for_flush_and_compaction=true（wiki 示例里就这么用）(GitHub)

---
4. Step 1：建库 / 填充数据（S 档）
先把 DB 填满，然后后续所有 workload 都用 --use_existing_db=1，避免反复写 SSD。
4.1 Fill（随机写入，模拟一般 KV）
$DB_BENCH \
  --db=$DB_DIR --wal_dir=$WAL_DIR \
  --benchmarks=fillrandom,stats --statistics \
  --num=50000000 \
  --key_size=16 --value_size=256 \
  --threads=16 \
  --compression_type=zstd \
  --cache_size=$((4<<30)) \
  --use_direct_reads=true \
  --use_direct_io_for_flush_and_compaction=true
说明
- 这里 cache_size 只是初始；你后面跑读/scan 时会扫 cache 档位。
- compression_type 你可换成 snappy/lz4/zstd/none，但务必在 A/B 对比中保持一致。

---
5. Step 2：Realistic 主负载（mixgraph）命令模板
5.1 Realistic mixgraph（短 scan 主体 + locality）
# 以 S 档为例：NUM_KEYS=50M，总请求数 reads=200M（约 20-40 分钟，视机器而定）
$DB_BENCH \
  --db=$DB_DIR --wal_dir=$WAL_DIR \
  --use_existing_db=1 \
  --benchmarks=mixgraph,stats --statistics \
  --num=50000000 \
  --reads=200000000 \
  --threads=16 \
  --key_size=16 \
  --cache_size=$((8<<30)) \
  --use_direct_reads=true \
  --use_direct_io_for_flush_and_compaction=true \
  \
  # value size: Generalized Pareto (按 wiki 要求提供) :contentReference[oaicite:15]{index=15}
  -value_k=0.9 -value_sigma=256 -value_theta=0 \
  \
  # key hotness: power distribution (按 wiki 要求提供) :contentReference[oaicite:16]{index=16}
  -key_dist_a=0.0016 -key_dist_b=-0.71 \
  \
  # key-range hotness: two-term exp + keyrange_num (locality 关键) :contentReference[oaicite:17]{index=17}
  -keyrange_dist_a=14.18 -keyrange_dist_b=-2.917 \
  -keyrange_dist_c=0.0164 -keyrange_dist_d=-0.08082 \
  -keyrange_num=32 \
  \
  # iterator scan length: Generalized Pareto (把 sigma/k 调小→更短的 scan 倾向) :contentReference[oaicite:18]{index=18}
  -iter_k=0.08 -iter_sigma=1.75 -iter_theta=0 \
  \
  # 操作混比：Seek 为主（短 scan 主体），少量 Get/Put
  -mix_get_ratio=0.10 -mix_put_ratio=0.05 -mix_seek_ratio=0.85

- UDB 事实约束是 >60% scan_len=1 + 200 台阶。
- mixgraph 的 iter 分布用的是连续分布（Pareto），很难天然长出“200 jump”。所以我们用下一步的 seekrandom 注入来补台阶（不改代码）。

---
6. Step 3：注入“200 台阶”（seekrandom + seek_nexts=200）
db_bench 的 seekrandom 定义是：随机 seeks，并对每次 seek 调用 Next seek_nexts 次。(GitHub)
这正好模拟 UDB 里来自上层 limit 的 200 台阶。
6.1 单次台阶注入（短时间）
# 例如注入 5M 次 seek，每次 Next 200（强台阶）
$DB_BENCH \
  --db=$DB_DIR --use_existing_db=1 \
  --benchmarks=seekrandom,stats --statistics \
  --num=50000000 \
  --reads=5000000 \
  --threads=16 \
  --key_size=16 \
  --cache_size=$((8<<30)) \
  --use_direct_reads=true \
  --use_direct_io_for_flush_and_compaction=true \
  --seek_nexts=200
6.2 “轻量混合”模板（不用并发，只用循环分段实现混合）
如果你不想两进程并发（并发会让结果更难复现）最稳的是用一个循环把“主体 + 台阶”按时间片混起来：
# 例：每个循环：跑 90 秒 mixgraph + 跑 10 秒 seek200，循环 30 次 ≈ 50 分钟
for i in $(seq 1 30); do
  $DB_BENCH \
    --db=$DB_DIR --wal_dir=$WAL_DIR --use_existing_db=1 \
    :contentReference[oaicite:22]{index=22}istics \
    --num=50000000 --reads=2000000 --threads=16 --key_size=16 \
    --cache_size=$((8<<30)) \
    --use_direct_reads=true --use_direct_io_for_flush_and_compaction=true \
    -value_k=0.9 -value_sigma=256 -value_theta=0 \
    -key_dist_a=0.0016 -key_dist_b=-0.71 \
    -keyrange_dist_a=14.18 -keyrange_dist_b=-2.917 \
    -ke:contentReference[oaicite:23]{index=23}e_dist_d=-0.08082 -keyrange_num=32 \
    -iter_k=0.08 -iter_sigma=1.75 -iter_theta=0 \
    -mix_get_ratio=0.10 -mix_put_ratio=0.05 -mix_seek_ratio=0.85

  $DB_BENCH \
    --db=$DB_DIR --use_existing_db=1 \
    --benchmarks=seekrandom --statistics \
    --num=50000000 --reads=200000 --threads=16 --key_size=16 \
    --cache_size=$((8<<30)) \
    --use_direct_reads=true --use_direct_io_for_flush_and_compaction=true \
    --seek_nexts=200
done

# 最后补一次 stats 汇总（可选）
$DB_BENCH --db=$DB_DIR --use_existing_db=1 --benchmarks=stats --statistics
这个“90% 主体 + 10% 台阶”的比例，你可以按实验 C（0/5/10/20%）去扫，来复现论文提到的 “200 jump”敏感性。

---
7. Worst locality 对照负载（YCSB-style：全局随机 key-space）
这里不追求 key-range locality（相当于把热点“打碎”），用 seekrandom 分段混合 scan length，构造“短为主 + 台阶 + 长尾”。这样完全无需定制，而且能作为“最坏情况”对照。
7.1 Worst locality：短 scan 主体（seek_nexts=1 占大头）
# 65% 时间：seek_nexts=1
$DB_BENCH \
  --db=$DB_DIR --use_existing_db=1 \
  --benchmarks=seekrandom,stats --statistics \
  --num=50000000 \
  --reads=65000000 \
  --threads=16 \
  --key_size=16 \
  --cache_size=$((8<<30)) \
  --use_direct_reads=true \
  --seek_nexts=1
7.2 Worst locality：补中等 scan（2–5、6–50）
# 20%：seek_nexts=4
$DB_BENCH --db=$DB_DIR --use_existing_db=1 --benchmarks=seekrandom --statistics \
  --num=50000000 --reads=20000000 --threads=16 --key_size=16 \
  --cache_size=$((8<<30)) --use_direct_reads=true --seek_nexts=4

# 10%：seek_nexts=20
$DB_BENCH --db=$DB_DIR --use_existing_db=1 --benchmarks=seekrandom --statistics \
  --num=50000000 --reads=10000000 --threads=16 --key_size=16 \
  --cache_size=$((8<<30)):contentReference[oaicite:25]{index=25}ek_nexts=20
7.3 Worst locality：注入 200 台阶 + 极少长尾
# 4%：seek_nexts=200 (台阶)
$DB_BENCH --db=$DB_DIR --use_existing_db=1 --benchmarks=seekrandom --statistics \
  --num=50000000 --reads=4000000 --threads=16 --key_size=16 \
  --cache_size=$((8<<30)) --use_direct_reads=true --seek_nexts=200

# 1%：seek_nexts=10000 (极少长尾)
$DB_BENCH --db=$DB_DIR --use_existing_db=1 --benchmarks=seekrandom --statistics \
  --num=50000000 --reads=1000000 --threads=8 --key_size=16 \
  --cache_size=$((8<<30)) --use_direct_reads=true --seek_nexts=10000
这组分段命令的“比例”对应你之前讨论的结构性约束（短为主 + 200 台阶 + 极少长尾），虽然不是精确复现论文 CDF，但对于“对照/最坏情况”已经很够用，而且完全不需要改 mixgraph 模型参数。

---
8. Cache size 扫描
把 --cache_size 分别替换为：
- 4GB = $((4<<30))
- 8GB = $((8<<30))
- 16GB = $((16<<30))
对 Realistic 主负载（mixgraph + 台阶注入）跑一遍三档 cache：你会得到很直观的“命中率/读放大/尾延迟曲线”。

---
9. 你在对比“旧格式 vs 新格式”时怎么做
1. 同一个 fill 出来的 DB：不要每次重建（避免 compaction 形状不一致）
2. 变量只改“文件格式开关/参数”，其他所有 RocksDB 参数固定
3. 每个点跑 3 次取中位数
4. 输出必须带：
  - p50/p95/p99/p999（seek/iterator）
  - block cache hit/miss、read-bytes、block reads（这是论文用来判断像不像真的关键 I/O 指标）

---
