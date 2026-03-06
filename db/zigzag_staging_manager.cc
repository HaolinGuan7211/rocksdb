#include "db/zigzag_staging_manager.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

#include "db/column_family.h"
#include "db/version_set.h"

namespace ROCKSDB_NAMESPACE {

namespace {

uint64_t SaturatingAdd(uint64_t a, uint64_t b) {
  if (std::numeric_limits<uint64_t>::max() - a < b) {
    return std::numeric_limits<uint64_t>::max();
  }
  return a + b;
}

}  // namespace

bool ZigZagStagingManager::ShouldProcessCompaction(const Compaction& c) const {
  const auto& iopts = c.immutable_options();
  if (!iopts.zigzag_staging_enabled) {
    return false;
  }
  const int min_source_level = iopts.zigzag_staging_source_level;
  const int max_source_level =
      std::max(min_source_level,
               std::min(iopts.zigzag_staging_max_source_level,
                        iopts.num_levels - 2));
  if (c.start_level() < min_source_level ||
      c.start_level() > max_source_level) {
    return false;
  }
  if (c.num_input_levels() == 0) {
    return false;
  }
  return true;
}

ZigZagStagingManager::MigrationSummary ZigZagStagingManager::OnCompactionPicked(
    const Compaction& c, uint64_t now_micros) {
  MigrationSummary summary;
  if (!ShouldProcessCompaction(c)) {
    return summary;
  }

  const auto& iopts = c.immutable_options();
  const int source_level = c.start_level();
  const int target_level = source_level + 1;

  StageLevelState& stage_level = GetOrInitStageLevel(c, source_level);
  RegisterShadowPartitions(stage_level, c, target_level);

  // MVP no-merge migration: account compaction start-level input files into
  // staging partitions, while normal compaction execution continues unchanged.
  const auto* start_level_inputs = c.inputs(0);
  for (const auto* f : *start_level_inputs) {
    const std::string smallest = ExtractSmallestUserKey(*f);
    const std::string largest = ExtractLargestUserKey(*f);
    const std::string range_key = MakeRangeKey(smallest, largest);

    auto it = stage_level.partition_ids_by_range.find(range_key);
    bool is_shadow = false;
    uint64_t shadow_file_number = 0;
    if (it == stage_level.partition_ids_by_range.end()) {
      const uint64_t pid = FindOrCreatePartitionId(stage_level, smallest,
                                                   largest, false, 0);
      it = stage_level.partition_ids_by_range.find(range_key);
      (void)pid;
    } else {
      const StagePartition& existing = stage_level.partitions[it->second];
      is_shadow = existing.is_shadow;
      shadow_file_number = existing.shadow_file_number;
    }

    const uint64_t partition_id = FindOrCreatePartitionId(
        stage_level, smallest, largest, is_shadow, shadow_file_number);
    StagePartition& p = GetPartition(stage_level, partition_id);

    p.bytes_used = SaturatingAdd(p.bytes_used, f->fd.GetFileSize());
    p.num_entries = SaturatingAdd(p.num_entries, PickEntryEstimate(*f));
    p.last_access_micros = now_micros;

    summary.migrated_files++;
    summary.migrated_bytes =
        SaturatingAdd(summary.migrated_bytes, f->fd.GetFileSize());
  }

  summary.touched_partitions = stage_level.partitions.size();

  const auto candidates = PickFlushCandidates(
      stage_level, iopts.zigzag_staging_level_capacity_bytes,
      iopts.zigzag_staging_partition_flush_threshold_bytes);
  summary.flushed_partitions = candidates.size();
  summary.flushed_bytes = FlushSelected(stage_level, candidates, now_micros);
  return summary;
}

size_t ZigZagStagingManager::GetPartitionCount(uint32_t cf_id,
                                               int stage_source_level) const {
  auto cf_it = cf_states_.find(cf_id);
  if (cf_it == cf_states_.end()) {
    return 0;
  }
  auto stage_it = cf_it->second.stage_levels.find(stage_source_level);
  if (stage_it == cf_it->second.stage_levels.end()) {
    return 0;
  }
  return stage_it->second.partitions.size();
}

uint64_t ZigZagStagingManager::FindOrCreatePartition(
    uint32_t cf_id, int stage_source_level, const std::string& smallest,
    const std::string& largest, bool is_shadow, uint64_t shadow_file_number) {
  StageLevelState& stage_level = cf_states_[cf_id].stage_levels[stage_source_level];
  return FindOrCreatePartitionId(stage_level, smallest, largest, is_shadow,
                                 shadow_file_number);
}

void ZigZagStagingManager::RegisterStagedFile(
    uint32_t cf_id, int stage_source_level, uint64_t partition_id,
    const FileMetaData& file, const std::string& staged_path,
    const std::string& original_db_path, uint64_t now_micros) {
  StageLevelState& stage_level =
      cf_states_[cf_id].stage_levels[stage_source_level];
  StagePartition& partition = GetPartition(stage_level, partition_id);
  StagedFile staged_file;
  staged_file.partition_id = partition_id;
  staged_file.metadata = file;
  staged_file.path = staged_path;
  staged_file.original_db_path = original_db_path;
  staged_file.file_number = file.fd.GetNumber();
  staged_file.largest_seqno = file.fd.largest_seqno;
  staged_file.smallest_user_key = ExtractSmallestUserKey(file);
  staged_file.largest_user_key = ExtractLargestUserKey(file);
  staged_file.is_shadow = partition.is_shadow;
  staged_file.shadow_file_number = partition.shadow_file_number;
  partition.staged_files.push_back(std::move(staged_file));
  partition.bytes_used =
      SaturatingAdd(partition.bytes_used, file.fd.GetFileSize());
  partition.num_entries =
      SaturatingAdd(partition.num_entries, PickEntryEstimate(file));
  partition.last_access_micros = now_micros;
}

std::vector<ZigZagStagingManager::StagedFile>
ZigZagStagingManager::FindCandidateFiles(uint32_t cf_id) const {
  std::vector<StagedFile> files;
  auto cf_it = cf_states_.find(cf_id);
  if (cf_it == cf_states_.end()) {
    return files;
  }
  for (const auto& stage_kv : cf_it->second.stage_levels) {
    for (const auto& partition_kv : stage_kv.second.partitions) {
      for (const auto& staged_file : partition_kv.second.staged_files) {
        files.push_back(staged_file);
      }
    }
  }
  std::sort(files.begin(), files.end(),
            [](const StagedFile& lhs, const StagedFile& rhs) {
              if (lhs.largest_seqno == rhs.largest_seqno) {
                return lhs.file_number > rhs.file_number;
              }
              return lhs.largest_seqno > rhs.largest_seqno;
            });
  return files;
}

std::vector<ZigZagStagingManager::StagedFile>
ZigZagStagingManager::GetFlushableFiles(uint32_t cf_id, int stage_source_level,
                                        uint64_t per_partition_threshold) const {
  std::vector<StagedFile> files;
  auto cf_it = cf_states_.find(cf_id);
  if (cf_it == cf_states_.end()) {
    return files;
  }
  auto stage_it = cf_it->second.stage_levels.find(stage_source_level);
  if (stage_it == cf_it->second.stage_levels.end()) {
    return files;
  }
  for (const auto& partition_kv : stage_it->second.partitions) {
    const StagePartition& partition = partition_kv.second;
    if (partition.bytes_used < per_partition_threshold) {
      continue;
    }
    files.insert(files.end(), partition.staged_files.begin(),
                 partition.staged_files.end());
  }
  return files;
}

void ZigZagStagingManager::RemoveStagedFile(uint32_t cf_id, int stage_source_level,
                                            uint64_t file_number) {
  auto cf_it = cf_states_.find(cf_id);
  if (cf_it == cf_states_.end()) {
    return;
  }
  auto stage_it = cf_it->second.stage_levels.find(stage_source_level);
  if (stage_it == cf_it->second.stage_levels.end()) {
    return;
  }
  for (auto& partition_kv : stage_it->second.partitions) {
    StagePartition& partition = partition_kv.second;
    auto& files = partition.staged_files;
    auto it = std::find_if(files.begin(), files.end(),
                           [file_number](const StagedFile& staged_file) {
                             return staged_file.file_number == file_number;
                           });
    if (it == files.end()) {
      continue;
    }
    const uint64_t file_size = it->metadata.fd.GetFileSize();
    const uint64_t entry_estimate = PickEntryEstimate(it->metadata);
    if (partition.bytes_used >= file_size) {
      partition.bytes_used -= file_size;
    } else {
      partition.bytes_used = 0;
    }
    if (partition.num_entries >= entry_estimate) {
      partition.num_entries -= entry_estimate;
    } else {
      partition.num_entries = 0;
    }
    files.erase(it);
    break;
  }
}

ZigZagStagingManager::StageLevelState& ZigZagStagingManager::GetOrInitStageLevel(
    const Compaction& c, int stage_source_level) {
  const uint32_t cf_id = c.column_family_data()->GetID();
  return cf_states_[cf_id].stage_levels[stage_source_level];
}

std::string ZigZagStagingManager::MakeRangeKey(const std::string& smallest,
                                                const std::string& largest) {
  std::string key;
  key.reserve(smallest.size() + largest.size() + 1);
  key.append(smallest);
  key.push_back('\0');
  key.append(largest);
  return key;
}

std::string ZigZagStagingManager::ExtractSmallestUserKey(
    const FileMetaData& file) {
  return file.smallest.user_key().ToString();
}

std::string ZigZagStagingManager::ExtractLargestUserKey(const FileMetaData& file) {
  return file.largest.user_key().ToString();
}

uint64_t ZigZagStagingManager::PickEntryEstimate(const FileMetaData& file) {
  // We keep an intentionally cheap approximation for scheduler scoring.
  return std::max<uint64_t>(1, file.fd.GetFileSize() / 128);
}

void ZigZagStagingManager::RegisterShadowPartitions(StageLevelState& stage_level,
                                                     const Compaction& c,
                                                     int target_ssd_level) const {
  if (target_ssd_level < 0) {
    return;
  }
  auto* vstorage = c.input_version()->storage_info();
  if (target_ssd_level >= vstorage->num_levels()) {
    return;
  }
  const auto& level_files = vstorage->LevelFiles(target_ssd_level);
  for (const auto* f : level_files) {
    const std::string smallest = ExtractSmallestUserKey(*f);
    const std::string largest = ExtractLargestUserKey(*f);
    const std::string range_key = MakeRangeKey(smallest, largest);
    if (stage_level.partition_ids_by_range.find(range_key) !=
        stage_level.partition_ids_by_range.end()) {
      continue;
    }
    uint64_t pid = stage_level.next_partition_id++;
    StagePartition p;
    p.partition_id = pid;
    p.smallest_user_key = smallest;
    p.largest_user_key = largest;
    p.is_shadow = true;
    p.shadow_file_number = f->fd.GetNumber();
    stage_level.partition_ids_by_range.emplace(range_key, pid);
    stage_level.partitions.emplace(pid, std::move(p));
  }
}

uint64_t ZigZagStagingManager::FindOrCreatePartitionId(
    StageLevelState& stage_level, const std::string& smallest,
    const std::string& largest, bool is_shadow, uint64_t shadow_file_number) {
  const std::string range_key = MakeRangeKey(smallest, largest);
  auto it = stage_level.partition_ids_by_range.find(range_key);
  if (it != stage_level.partition_ids_by_range.end()) {
    return it->second;
  }

  const uint64_t pid = stage_level.next_partition_id++;
  StagePartition partition;
  partition.partition_id = pid;
  partition.smallest_user_key = smallest;
  partition.largest_user_key = largest;
  partition.is_shadow = is_shadow;
  partition.shadow_file_number = shadow_file_number;

  stage_level.partition_ids_by_range.emplace(range_key, pid);
  stage_level.partitions.emplace(pid, std::move(partition));
  return pid;
}

ZigZagStagingManager::StagePartition& ZigZagStagingManager::GetPartition(
    StageLevelState& stage_level, uint64_t partition_id) {
  return stage_level.partitions[partition_id];
}

std::vector<ZigZagStagingManager::StageCandidate>
ZigZagStagingManager::PickFlushCandidates(
    const StageLevelState& stage_level, uint64_t capacity_limit,
    uint64_t per_partition_flush_threshold) const {
  std::vector<StageCandidate> candidates;
  uint64_t total_bytes = 0;
  for (const auto& kv : stage_level.partitions) {
    total_bytes = SaturatingAdd(total_bytes, kv.second.bytes_used);
    if (kv.second.bytes_used >= per_partition_flush_threshold) {
      StageCandidate c;
      c.partition_id = kv.first;
      c.score = kv.second.bytes_used;
      candidates.push_back(c);
    }
  }

  if (total_bytes <= capacity_limit) {
    std::sort(candidates.begin(), candidates.end(),
              [](const StageCandidate& a, const StageCandidate& b) {
                return a.partition_id < b.partition_id;
              });
    return candidates;
  }

  // Capacity trigger: greedily flush bigger partitions first.
  std::vector<StageCandidate> all;
  all.reserve(stage_level.partitions.size());
  for (const auto& kv : stage_level.partitions) {
    StageCandidate c;
    c.partition_id = kv.first;
    c.score = kv.second.bytes_used;
    all.push_back(c);
  }
  std::sort(all.begin(), all.end(), [](const StageCandidate& a,
                                       const StageCandidate& b) {
    if (a.score == b.score) {
      return a.partition_id < b.partition_id;
    }
    return a.score > b.score;
  });

  uint64_t over = total_bytes - capacity_limit;
  for (const auto& c : all) {
    candidates.push_back(c);
    if (over <= c.score) {
      break;
    }
    over -= c.score;
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const StageCandidate& a, const StageCandidate& b) {
              return a.partition_id < b.partition_id;
            });
  candidates.erase(
      std::unique(candidates.begin(), candidates.end(),
                  [](const StageCandidate& a, const StageCandidate& b) {
                    return a.partition_id == b.partition_id;
                  }),
      candidates.end());
  return candidates;
}

uint64_t ZigZagStagingManager::FlushSelected(
    StageLevelState& stage_level, const std::vector<StageCandidate>& candidates,
    uint64_t now_micros) {
  uint64_t flushed = 0;
  for (const auto& c : candidates) {
    auto it = stage_level.partitions.find(c.partition_id);
    if (it == stage_level.partitions.end()) {
      continue;
    }
    StagePartition& p = it->second;
    flushed = SaturatingAdd(flushed, p.bytes_used);
    p.bytes_used = 0;
    p.num_entries = 0;
    p.last_access_micros = now_micros;
  }
  return flushed;
}

}  // namespace ROCKSDB_NAMESPACE
