# ZigZag Staging MVP Benchmark Workflow

## 1. 目标
- 在不改 SST/WAL/MANIFEST 文件格式前提下，验证 ZigZag staging MVP 的功能可用性与收益方向。
- 对照项：`zigzag_staging_enabled=0`（baseline） vs `zigzag_staging_enabled=1`（zigzag-mvp）。

## 2. 适用范围
- 单 CF。
- Level Compaction。
- 先验证 `L0 -> L0.5`（`zigzag_staging_source_level=0`）。
- 多半层功能验证可扩展到 `L1.5/L2.5...`，通过 `zigzag_staging_max_source_level` 控制上界。

## 3. 构建与环境要求
- 使用 Release 构建。
- 低资源机器（4C/8G）建议单线程编译：`make -j1 db_bench`。
- 若环境不支持 `/dev/shm`，避免将 DB 路径放入 tmpfs。
- 功能回归建议同时保留 `TMPDIR=/tmp TEST_TMPDIR=/tmp ./zigzag_staging_test`。

## 4. Case 矩阵（最小集合）
1. `A1_baseline`：staging 关闭。
2. `A2_zigzag_default`：staging 开启，默认阈值。
3. `A3_zigzag_small_threshold`：staging 开启，`zigzag_staging_partition_flush_threshold_bytes` 下调到 `16MB`。
4. `B1_multi_half_functional`：功能回归，验证 `L0.5/L1.5(/L2.5)` 级联迁移与恢复。

## 5. 固定参数建议
- `benchmarks=fillrandom,stats,overwrite,stats`
- `num=2000000`
- `value_size=128`
- `key_size=16`
- `threads=4`
- `level_compaction_dynamic_level_bytes=true`
- `compression_type=none`
- `target_file_size_base=67108864`
- `write_buffer_size=67108864`
- `max_background_jobs=4`
- `statistics=true`
- `stats_interval_seconds=15`

## 6. 运行命令模板
```bash
./db_bench \
  --db=/tmp/rocksdb_zigzag_mvp_A1 \
  --benchmarks=fillrandom,stats,overwrite,stats \
  --num=2000000 --key_size=16 --value_size=128 --threads=4 \
  --compression_type=none \
  --write_buffer_size=67108864 --target_file_size_base=67108864 \
  --level_compaction_dynamic_level_bytes=true \
  --max_background_jobs=4 --statistics=true --stats_interval_seconds=15 \
  --zigzag_staging_enabled=false
```

```bash
./db_bench \
  --db=/tmp/rocksdb_zigzag_mvp_A2 \
  --benchmarks=fillrandom,stats,overwrite,stats \
  --num=2000000 --key_size=16 --value_size=128 --threads=4 \
  --compression_type=none \
  --write_buffer_size=67108864 --target_file_size_base=67108864 \
  --level_compaction_dynamic_level_bytes=true \
  --max_background_jobs=4 --statistics=true --stats_interval_seconds=15 \
  --zigzag_staging_enabled=true \
  --zigzag_staging_source_level=0 \
  --zigzag_staging_level_capacity_bytes=4294967296 \
  --zigzag_staging_partition_flush_threshold_bytes=67108864
```

```bash
./db_bench \
  --db=/tmp/rocksdb_zigzag_mvp_A3 \
  --benchmarks=fillrandom,stats,overwrite,stats \
  --num=2000000 --key_size=16 --value_size=128 --threads=4 \
  --compression_type=none \
  --write_buffer_size=67108864 --target_file_size_base=67108864 \
  --level_compaction_dynamic_level_bytes=true \
  --max_background_jobs=4 --statistics=true --stats_interval_seconds=15 \
  --zigzag_staging_enabled=true \
  --zigzag_staging_source_level=0 \
  --zigzag_staging_level_capacity_bytes=4294967296 \
  --zigzag_staging_partition_flush_threshold_bytes=16777216
```

```bash
TMPDIR=/tmp TEST_TMPDIR=/tmp ./zigzag_staging_test \
  --gtest_filter='ZigZagStagingTest.MultiHalfLevelCascadeFlushesAcrossLevels:ZigZagStagingTest.DeepMultiHalfLevelCascadeFlushesToL3'
```

多半层 bench smoke 可在 A2/A3 的基础上增加：

```bash
  --zigzag_staging_max_source_level=1
```

或：

```bash
  --zigzag_staging_max_source_level=2
```

说明：
- `db_bench` 小规模 smoke 更适合验证 `L0 -> L0.5 -> L1` 行为与吞吐方向。
- 更深层的 `L1.5/L2.5` 级联建议用上面的 `zigzag_staging_test` 或手工 `CompactFiles()` 验证。

## 7. 结果采集（最小）
- 吞吐：ops/sec。
- 尾延迟：p99、p99.9（overwrite phase）。
- 写放大：`rocksdb.stats` 中 compaction write/read bytes。
- 事件日志：检索关键字 `ZigZag staged file`、`ZigZag flushed staged file`、`ZigZag shadow-merged staged file`。

## 8. 判定标准
- 功能正确：开启 staging 后无 crash、无一致性错误、可完成全流程。
- 方向正确：A2/A3 相比 A1 至少在一个维度有改善（吞吐或尾延迟或写放大）。
- 多半层正确：`B1` 中 `L0.5/L1.5(/L2.5)` 级联迁移后读结果与 reopen 后结果一致。
- 若收益不稳定：保留日志并回填 `experiment_result.md` 的异常解释。
