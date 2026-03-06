//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "db/db_test_util.h"

#include <string>
#include <vector>

#include "rocksdb/metadata.h"

namespace ROCKSDB_NAMESPACE {

class ZigZagStagingTest : public DBTestBase {
 public:
  ZigZagStagingTest()
      : DBTestBase("zigzag_staging_test", /* env_do_fsync */ false) {}

  Options ZigZagOptions(int source_level, int max_source_level,
                        uint64_t flush_threshold_bytes) {
    Options options = CurrentOptions();
    options.create_if_missing = true;
    options.disable_auto_compactions = true;
    options.level_compaction_dynamic_level_bytes = false;
    options.num_levels = 4;
    options.write_buffer_size = 4096;
    options.target_file_size_base = 4096;
    options.max_bytes_for_level_base = 16384;
    options.level0_file_num_compaction_trigger = 100;
    options.max_background_jobs = 1;
    options.compression = kNoCompression;
    options.zigzag_staging_enabled = true;
    options.zigzag_staging_source_level = source_level;
    options.zigzag_staging_max_source_level = max_source_level;
    options.zigzag_staging_level_capacity_bytes = 1 << 20;
    options.zigzag_staging_partition_flush_threshold_bytes =
        flush_threshold_bytes;
    return options;
  }

  std::vector<std::string> LevelFiles(int level) {
    std::vector<LiveFileMetaData> metadata;
    db_->GetLiveFilesMetaData(&metadata);
    std::vector<std::string> result;
    for (const auto& file : metadata) {
      if (file.level == level) {
        result.push_back(file.db_path + "/" + file.name);
      }
    }
    return result;
  }

  void CompactFilesToLevel(const std::vector<std::string>& files,
                           int output_level) {
    CompactionOptions options;
    options.compression = kDisableCompressionOption;
    ASSERT_OK(db_->CompactFiles(options, files, output_level));
  }
};

TEST_F(ZigZagStagingTest, StageOnlyReadsRespectRangeTombstoneAndRecover) {
  const uint64_t kStageOnlyThreshold = 1ULL << 30;
  DestroyAndReopen(ZigZagOptions(/*source_level=*/0, /*max_source_level=*/0,
                                 kStageOnlyThreshold));

  ASSERT_OK(Put("a", "va"));
  ASSERT_OK(Put("c", "vc"));
  ASSERT_OK(db_->DeleteRange(WriteOptions(), db_->DefaultColumnFamily(), "a",
                             "b"));
  ASSERT_OK(Flush());

  auto l0_files = LevelFiles(0);
  ASSERT_EQ(1U, l0_files.size());
  CompactFilesToLevel(l0_files, 1);

  ASSERT_EQ(0, NumTableFilesAtLevel(0));
  ASSERT_EQ(0, NumTableFilesAtLevel(1));
  ASSERT_EQ("NOT_FOUND", Get("a"));
  ASSERT_EQ("vc", Get("c"));
  ASSERT_EQ((std::vector<std::string>{"NOT_FOUND", "vc"}),
            MultiGet({"a", "c"}));

  ReadOptions ro;
  std::unique_ptr<Iterator> iter(db_->NewIterator(ro));
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ("c", iter->key().ToString());
  ASSERT_EQ("vc", iter->value().ToString());
  iter->Next();
  ASSERT_FALSE(iter->Valid());
  iter.reset();

  Close();
  Reopen(ZigZagOptions(/*source_level=*/0, /*max_source_level=*/0,
                       kStageOnlyThreshold));

  ASSERT_EQ("NOT_FOUND", Get("a"));
  ASSERT_EQ("vc", Get("c"));
  iter.reset(db_->NewIterator(ro));
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ("c", iter->key().ToString());
  iter->Next();
  ASSERT_FALSE(iter->Valid());
}

TEST_F(ZigZagStagingTest, StageOnlyCloseAfterCompact) {
  const uint64_t kStageOnlyThreshold = 1ULL << 30;
  DestroyAndReopen(ZigZagOptions(/*source_level=*/0, /*max_source_level=*/0,
                                 kStageOnlyThreshold));

  ASSERT_OK(Put("a", "va"));
  ASSERT_OK(Put("c", "vc"));
  ASSERT_OK(Flush());

  auto l0_files = LevelFiles(0);
  ASSERT_EQ(1U, l0_files.size());
  CompactFilesToLevel(l0_files, 1);

  ASSERT_EQ(0, NumTableFilesAtLevel(0));
  ASSERT_EQ(0, NumTableFilesAtLevel(1));
  Close();
}

TEST_F(ZigZagStagingTest, StageOnlyCloseAfterIterator) {
  const uint64_t kStageOnlyThreshold = 1ULL << 30;
  DestroyAndReopen(ZigZagOptions(/*source_level=*/0, /*max_source_level=*/0,
                                 kStageOnlyThreshold));

  ASSERT_OK(Put("a", "va"));
  ASSERT_OK(Put("c", "vc"));
  ASSERT_OK(db_->DeleteRange(WriteOptions(), db_->DefaultColumnFamily(), "a",
                             "b"));
  ASSERT_OK(Flush());

  auto l0_files = LevelFiles(0);
  ASSERT_EQ(1U, l0_files.size());
  CompactFilesToLevel(l0_files, 1);

  ReadOptions ro;
  std::unique_ptr<Iterator> iter(db_->NewIterator(ro));
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ("c", iter->key().ToString());
  iter->Next();
  ASSERT_FALSE(iter->Valid());
  iter.reset();
  Close();
}

TEST_F(ZigZagStagingTest, DestroyAndReopenClearsStaleStaging) {
  const uint64_t kStageOnlyThreshold = 1ULL << 30;
  DestroyAndReopen(ZigZagOptions(/*source_level=*/0, /*max_source_level=*/0,
                                 kStageOnlyThreshold));

  ASSERT_OK(Put("stale", "vs"));
  ASSERT_OK(Flush());

  auto l0_files = LevelFiles(0);
  ASSERT_EQ(1U, l0_files.size());
  CompactFilesToLevel(l0_files, 1);

  ASSERT_EQ("vs", Get("stale"));

  DestroyAndReopen(ZigZagOptions(/*source_level=*/0, /*max_source_level=*/0,
                                 kStageOnlyThreshold));
  ASSERT_EQ("NOT_FOUND", Get("stale"));
  ASSERT_EQ(0, NumTableFilesAtLevel(0));
  ASSERT_EQ(0, NumTableFilesAtLevel(1));
}

TEST_F(ZigZagStagingTest, MultiInputCompactionStagesOneEligibleFile) {
  const uint64_t kStageOnlyThreshold = 1ULL << 30;
  DestroyAndReopen(ZigZagOptions(/*source_level=*/0, /*max_source_level=*/0,
                                 kStageOnlyThreshold));

  ASSERT_OK(Put("a", "va"));
  ASSERT_OK(Flush());
  ASSERT_OK(Put("c", "vc"));
  ASSERT_OK(Flush());

  auto l0_files = LevelFiles(0);
  ASSERT_EQ(2U, l0_files.size());
  CompactFilesToLevel(l0_files, 1);

  ASSERT_EQ(1, NumTableFilesAtLevel(0));
  ASSERT_EQ(0, NumTableFilesAtLevel(1));
  ASSERT_EQ("va", Get("a"));
  ASSERT_EQ("vc", Get("c"));

  Close();
  Reopen(ZigZagOptions(/*source_level=*/0, /*max_source_level=*/0,
                       kStageOnlyThreshold));
  ASSERT_EQ("va", Get("a"));
  ASSERT_EQ("vc", Get("c"));
}

TEST_F(ZigZagStagingTest, ShadowMergePreservesRangeTombstone) {
  DestroyAndReopen(
      ZigZagOptions(/*source_level=*/0, /*max_source_level=*/0,
                    /*flush_threshold_bytes=*/1));

  ASSERT_OK(Put("a", "va0"));
  ASSERT_OK(Put("c", "vc0"));
  ASSERT_OK(db_->DeleteRange(WriteOptions(), db_->DefaultColumnFamily(), "a",
                             "b"));
  ASSERT_OK(Flush());
  auto first_l0_files = LevelFiles(0);
  ASSERT_EQ(1U, first_l0_files.size());
  CompactFilesToLevel(first_l0_files, 1);

  ASSERT_EQ(0, NumTableFilesAtLevel(0));
  ASSERT_EQ(1, NumTableFilesAtLevel(1));
  ASSERT_EQ("NOT_FOUND", Get("a"));
  ASSERT_EQ("vc0", Get("c"));

  ASSERT_OK(Put("a", "va1"));
  ASSERT_OK(Put("c", "vc1"));
  ASSERT_OK(db_->DeleteRange(WriteOptions(), db_->DefaultColumnFamily(), "a",
                             "b"));
  ASSERT_OK(Flush());
  auto second_l0_files = LevelFiles(0);
  ASSERT_EQ(1U, second_l0_files.size());
  CompactFilesToLevel(second_l0_files, 1);

  ASSERT_EQ(0, NumTableFilesAtLevel(0));
  ASSERT_EQ(1, NumTableFilesAtLevel(1));
  ASSERT_EQ("NOT_FOUND", Get("a"));
  ASSERT_EQ("vc1", Get("c"));
}

TEST_F(ZigZagStagingTest, MultiHalfLevelCascadeFlushesAcrossLevels) {
  auto options =
      ZigZagOptions(/*source_level=*/0, /*max_source_level=*/1,
                    /*flush_threshold_bytes=*/1);
  DestroyAndReopen(options);

  ASSERT_OK(Put("c", "vc"));
  ASSERT_OK(Flush());

  auto l0_files = LevelFiles(0);
  ASSERT_EQ(1U, l0_files.size());
  CompactFilesToLevel(l0_files, 1);

  ASSERT_EQ(0, NumTableFilesAtLevel(0));
  ASSERT_EQ(1, NumTableFilesAtLevel(1));
  ASSERT_EQ(0, NumTableFilesAtLevel(2));
  ASSERT_EQ("vc", Get("c"));

  auto l1_files = LevelFiles(1);
  ASSERT_EQ(1U, l1_files.size());
  CompactFilesToLevel(l1_files, 2);

  ASSERT_EQ(0, NumTableFilesAtLevel(0));
  ASSERT_EQ(0, NumTableFilesAtLevel(1));
  ASSERT_EQ(1, NumTableFilesAtLevel(2));
  ASSERT_EQ("vc", Get("c"));

  Close();
  Reopen(options);
  ASSERT_EQ("vc", Get("c"));
  ASSERT_EQ(1, NumTableFilesAtLevel(2));
}

TEST_F(ZigZagStagingTest, DeepMultiHalfLevelCascadeFlushesToL3) {
  auto options =
      ZigZagOptions(/*source_level=*/0, /*max_source_level=*/2,
                    /*flush_threshold_bytes=*/1);
  options.num_levels = 5;
  DestroyAndReopen(options);

  ASSERT_OK(Put("c", "vc"));
  ASSERT_OK(Flush());

  auto l0_files = LevelFiles(0);
  ASSERT_EQ(1U, l0_files.size());
  CompactFilesToLevel(l0_files, 1);
  ASSERT_EQ(1, NumTableFilesAtLevel(1));
  ASSERT_EQ("vc", Get("c"));

  auto l1_files = LevelFiles(1);
  ASSERT_EQ(1U, l1_files.size());
  CompactFilesToLevel(l1_files, 2);
  ASSERT_EQ(1, NumTableFilesAtLevel(2));
  ASSERT_EQ("vc", Get("c"));

  auto l2_files = LevelFiles(2);
  ASSERT_EQ(1U, l2_files.size());
  CompactFilesToLevel(l2_files, 3);
  ASSERT_EQ(0, NumTableFilesAtLevel(1));
  ASSERT_EQ(0, NumTableFilesAtLevel(2));
  ASSERT_EQ(1, NumTableFilesAtLevel(3));
  ASSERT_EQ("vc", Get("c"));

  Close();
  Reopen(options);
  ASSERT_EQ("vc", Get("c"));
  ASSERT_EQ(1, NumTableFilesAtLevel(3));
}

TEST_F(ZigZagStagingTest, MultiSourceLevelCanStageL1Compaction) {
  DestroyAndReopen(
      ZigZagOptions(/*source_level=*/0, /*max_source_level=*/0,
                    /*flush_threshold_bytes=*/1));

  ASSERT_OK(Put("c", "vc"));
  ASSERT_OK(Flush());
  auto l0_files = LevelFiles(0);
  ASSERT_EQ(1U, l0_files.size());
  CompactFilesToLevel(l0_files, 1);

  ASSERT_EQ(0, NumTableFilesAtLevel(0));
  ASSERT_EQ(1, NumTableFilesAtLevel(1));
  ASSERT_EQ("vc", Get("c"));

  Close();
  Reopen(ZigZagOptions(/*source_level=*/0, /*max_source_level=*/1,
                       /*flush_threshold_bytes=*/1ULL << 30));

  auto l1_files = LevelFiles(1);
  ASSERT_EQ(1U, l1_files.size());
  CompactFilesToLevel(l1_files, 2);

  ASSERT_EQ(0, NumTableFilesAtLevel(1));
  ASSERT_EQ(0, NumTableFilesAtLevel(2));
  ASSERT_EQ("vc", Get("c"));

  Close();
  Reopen(ZigZagOptions(/*source_level=*/0, /*max_source_level=*/1,
                       /*flush_threshold_bytes=*/1ULL << 30));
  ASSERT_EQ("vc", Get("c"));
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
