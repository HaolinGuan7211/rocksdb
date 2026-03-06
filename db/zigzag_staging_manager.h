#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "db/compaction/compaction.h"
#include "db/version_edit.h"
#include "rocksdb/slice.h"

namespace ROCKSDB_NAMESPACE {

class Logger;

class ZigZagStagingManager {
 public:
  struct StagedFile {
    uint64_t partition_id = 0;
    FileMetaData metadata;
    std::string path;
    std::string original_db_path;
    uint64_t file_number = 0;
    uint64_t largest_seqno = 0;
    std::string smallest_user_key;
    std::string largest_user_key;
    bool is_shadow = false;
    uint64_t shadow_file_number = 0;
  };

  struct StagePartition {
    uint64_t partition_id = 0;
    std::string smallest_user_key;
    std::string largest_user_key;
    bool is_shadow = false;
    uint64_t shadow_file_number = 0;

    uint64_t bytes_used = 0;
    uint64_t num_entries = 0;
    uint64_t read_count = 0;
    uint64_t read_miss_count = 0;
    uint64_t last_access_micros = 0;
    std::vector<StagedFile> staged_files;
  };

  struct MigrationSummary {
    uint64_t migrated_files = 0;
    uint64_t migrated_bytes = 0;
    uint64_t touched_partitions = 0;
    uint64_t flushed_partitions = 0;
    uint64_t flushed_bytes = 0;
  };

  ZigZagStagingManager() = default;

  bool ShouldProcessCompaction(const Compaction& c) const;

  MigrationSummary OnCompactionPicked(const Compaction& c,
                                      uint64_t now_micros);

  size_t GetPartitionCount(uint32_t cf_id, int stage_source_level) const;

  uint64_t FindOrCreatePartition(uint32_t cf_id, int stage_source_level,
                                 const std::string& smallest,
                                 const std::string& largest,
                                 bool is_shadow,
                                 uint64_t shadow_file_number);

  void RegisterStagedFile(uint32_t cf_id, int stage_source_level,
                          uint64_t partition_id, const FileMetaData& file,
                          const std::string& staged_path,
                          const std::string& original_db_path,
                          uint64_t now_micros);

  std::vector<StagedFile> FindCandidateFiles(uint32_t cf_id) const;

  std::vector<StagedFile> GetFlushableFiles(uint32_t cf_id,
                                            int stage_source_level,
                                            uint64_t per_partition_threshold) const;

  void RemoveStagedFile(uint32_t cf_id, int stage_source_level,
                        uint64_t file_number);

 private:
  struct StageLevelState {
    uint64_t next_partition_id = 1;
    // key: "<smallest>\0<largest>"
    std::unordered_map<std::string, uint64_t> partition_ids_by_range;
    std::map<uint64_t, StagePartition> partitions;
  };

  struct CfState {
    // key is source level i for stage i+0.5
    std::unordered_map<int, StageLevelState> stage_levels;
  };

  struct StageCandidate {
    uint64_t partition_id = 0;
    uint64_t score = 0;
  };

  StageLevelState& GetOrInitStageLevel(const Compaction& c,
                                       int stage_source_level);

  static std::string MakeRangeKey(const std::string& smallest,
                                  const std::string& largest);

  static std::string ExtractSmallestUserKey(const FileMetaData& file);
  static std::string ExtractLargestUserKey(const FileMetaData& file);

  static uint64_t PickEntryEstimate(const FileMetaData& file);

  void RegisterShadowPartitions(StageLevelState& stage_level,
                                const Compaction& c,
                                int target_ssd_level) const;

  uint64_t FindOrCreatePartitionId(StageLevelState& stage_level,
                                   const std::string& smallest,
                                   const std::string& largest,
                                   bool is_shadow,
                                   uint64_t shadow_file_number);

  StagePartition& GetPartition(StageLevelState& stage_level,
                               uint64_t partition_id);

  std::vector<StageCandidate> PickFlushCandidates(
      const StageLevelState& stage_level, uint64_t capacity_limit,
      uint64_t per_partition_flush_threshold) const;

  uint64_t FlushSelected(StageLevelState& stage_level,
                         const std::vector<StageCandidate>& candidates,
                         uint64_t now_micros);

  std::unordered_map<uint32_t, CfState> cf_states_;
};

}  // namespace ROCKSDB_NAMESPACE
