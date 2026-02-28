//  Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "rocksdb/file_system.h"

namespace ROCKSDB_NAMESPACE {

class SimulatedFsLatencyMonitor;

struct SimulatedStorageModelOptions {
  // XP-like NVM simulation model:
  // delay_ns = ceil(bytes / xp_line_bytes) * xp_latency_ns.
  bool use_xp_model = false;
  // Single-DIMM NVM model:
  // - A file-level simulator that injects a fixed overhead plus a bandwidth
  //   bounded transfer time.
  // - Intended to model one DIMM worth of NVM (one device / one channel).
  // - "GB/s" here uses decimal units (1 GB = 1e9 bytes), therefore
  //   `bw_gbps` is numerically equal to bytes/ns.
  bool use_dimm_model = false;
  uint64_t xp_line_bytes = 256;
  uint64_t xp_buffer_bytes = 16 * 1024;
  uint64_t xp_latency_ns = 300;
  uint64_t xp_rpq_depth = 64;
  uint64_t xp_wpq_depth = 64;
  uint64_t xp_wpq_submit_ns = 100;
  uint64_t xp_prefetch_hit_ns = 120;
  // DRAM-side baseline latency (paper anchors): seq=81ns, rand=101ns.
  uint64_t dram_read_seq_ns = 81;
  uint64_t dram_read_rand_ns = 101;
  bool xp_enable_prefetch = true;
  // Whether read prefetch and write merge share the same XPBuffer pipeline.
  bool xp_share_buffer_between_rw = true;
  // Parallelism controls for fitting macro bandwidth / concurrency curves.
  uint64_t xp_rpq_parallelism = 1;
  uint64_t xp_wpq_parallelism = 1;
  uint64_t xp_read_line_parallelism = 1;
  uint64_t xp_write_line_parallelism = 1;
  // Additional RPQ arbitration penalty per outstanding read request.
  // Used to model controller-side contention that grows with queue depth.
  uint64_t xp_rpq_arb_ns = 0;
  // Initial virtual-time stagger per forced deterministic stream tag.
  // Effective only when xp_bypass_base_io=true and profiling code sets forced
  // thread tags.
  uint64_t xp_forced_tag_init_stagger_ns = 0;
  // If true, bypass base filesystem IO in wrappers and only keep simulated
  // latency / queue behavior. Useful for pure model profiling.
  bool xp_bypass_base_io = false;

  // DIMM model parameters.
  // Target example (user requirement):
  // - 4KB read latency ~= 1.7us
  // - max read bandwidth ~= 6.6 GB/s
  // - max write bandwidth ~= 2.3 GB/s
  // - random 4KB bandwidth ~= 5% of sequential
  // Notes:
  // - Fixed overhead is applied per request (host-side path overhead) and
  //   contributes to latency but not to device transfer bandwidth.
  uint64_t dimm_fixed_read_overhead_ns = 1080;  // ~= 1.7us - 4KB/6.6GB/s
  uint64_t dimm_fixed_write_overhead_ns = 0;
  double dimm_seq_read_bw_gbps = 6.6;
  double dimm_seq_write_bw_gbps = 2.3;
  // Random access bandwidth multiplier.
  // Example: 0.95 means random bandwidth is 95% of sequential (5% drop).
  double dimm_rand_bw_scale = 0.95;
  // For random accesses smaller than xp_line_bytes, amplify media bytes to
  // enforce at least an 80% throughput degradation (<=20% effective
  // throughput) compared to an ideal byte-granularity device.
  double dimm_sub_line_random_media_amp = 5.0;

  // If true, files selected by XP simulation are persisted under
  // `xp_tmpfs_root` instead of the original path (logical path remains
  // unchanged to upper layers).
  bool xp_redirect_to_tmpfs = false;
  // Backing tmpfs root. On most systems /dev/shm is tmpfs and writable
  // unprivileged. If you really want /tmpfs, mount it explicitly and pass the
  // path via `--simulate_xp_tmpfs_root`.
  std::string xp_tmpfs_root = "/dev/shm/tmpfs";
  // Optional path filter. Empty means all files.
  std::string path_prefix;
  // Optional SST level filter. Empty means all levels.
  // When set, XP simulation is only applied to SST files whose current level
  // is in this set.
  std::unordered_set<int> target_levels;
  // Optional output path for model statistics (key=value).
  std::string stats_file;

  // Optional time-series latency monitor (CSV).
  // Intended for correlating workload bursts with storage-side tail latency.
  bool monitor_enable = false;
  // Fixed window size in microseconds. Typical: 1000000 (1s).
  uint64_t monitor_window_us = 1000000;
  // Optional stage window size in seconds (e.g., align to mix_shift_stage_seconds).
  // 0 disables stage aggregation output.
  uint64_t monitor_stage_seconds = 0;
  // Track per-window max latency for specific op types.
  bool monitor_max_read = true;
  bool monitor_max_open = true;
  bool monitor_max_prefetch = true;
  // Output CSV paths. Empty disables writing that stream.
  std::string monitor_window_csv;
  std::string monitor_stage_csv;
};

struct SimulatedStorageModelStats {
  std::atomic<uint64_t> logical_read_bytes{0};
  std::atomic<uint64_t> logical_write_bytes{0};
  std::atomic<uint64_t> media_read_bytes{0};
  std::atomic<uint64_t> media_write_bytes{0};
  std::atomic<uint64_t> read_ops{0};
  std::atomic<uint64_t> write_ops{0};
  std::atomic<uint64_t> simulated_read_delay_ns{0};
  std::atomic<uint64_t> simulated_write_delay_ns{0};
  std::atomic<uint64_t> simulated_read_fixed_delay_ns{0};
  std::atomic<uint64_t> simulated_write_fixed_delay_ns{0};
  std::atomic<uint64_t> simulated_read_queue_delay_ns{0};
  std::atomic<uint64_t> simulated_read_media_delay_ns{0};
  std::atomic<uint64_t> simulated_read_dram_delay_ns{0};
  std::atomic<uint64_t> simulated_read_arb_delay_ns{0};
  std::atomic<uint64_t> simulated_write_queue_delay_ns{0};
  std::atomic<uint64_t> simulated_write_media_delay_ns{0};
  std::atomic<uint64_t> read_prefetch_hits{0};
  std::atomic<uint64_t> xp_buffer_flushes{0};
  std::atomic<uint64_t> simulated_read_virtual_start_ns{0};
  std::atomic<uint64_t> simulated_read_virtual_end_ns{0};
  std::atomic<uint64_t> simulated_write_virtual_start_ns{0};
  std::atomic<uint64_t> simulated_write_virtual_end_ns{0};
  // When tmpfs redirection is enabled, count how many file opens are served
  // from tmpfs vs the base filesystem (best-effort, mainly for experiment
  // verification).
  std::atomic<uint64_t> tmpfs_read_opens{0};
  std::atomic<uint64_t> tmpfs_write_opens{0};

  // When tmpfs redirection is enabled, approximate how many read/prefetch ops
  // are served from tmpfs vs the base filesystem (based on resolved open path).
  std::atomic<uint64_t> tmpfs_read_ops{0};
  std::atomic<uint64_t> tmpfs_read_bytes{0};
  std::atomic<uint64_t> base_read_ops{0};
  std::atomic<uint64_t> base_read_bytes{0};
  std::atomic<uint64_t> tmpfs_prefetch_ops{0};
  std::atomic<uint64_t> tmpfs_prefetch_bytes{0};
  std::atomic<uint64_t> base_prefetch_ops{0};
  std::atomic<uint64_t> base_prefetch_bytes{0};
};

// Profiling-only utility: override the current thread's simulated stream tag.
// This is used by deterministic replay to emulate multi-thread logical streams
// while running on fewer OS threads.
void SetSimulatedFsThreadTagForCurrentThread(uint64_t tag);
void ClearSimulatedFsThreadTagForCurrentThread();
void RegisterSimulatedFsFileLevel(const std::string& file_name, int level);
void UnregisterSimulatedFsFileLevel(const std::string& file_name);
void ClearSimulatedFsFileLevels();

// A FileSystem simulates hybrid file system by ingesting latency and limit
// IOPs.
// This class is only used for development purpose and should not be used
// in production.
// Right now we ingest 15ms latency and allow 100 requests per second when
// the file is for warm temperature.
// When the object is destroyed, the list of warm files are written to a
// file, which can be used to reopen a FileSystem and still recover the
// list. This is to allow the information to preserve between db_bench
// runs.
class SimulatedHybridFileSystem : public FileSystemWrapper {
 public:
  // metadata_file_name stores metadata of the files, so that it can be
  // loaded after process restarts. If the file doesn't exist, create
  // one. The file is written when the class is destroyed.
  // throughput_multiplier: multiplier of throughput. For example, 1 is to
  //      simulate single disk spindle. 4 is to simualte 4 disk spindles.
  // is_full_fs_warm: if true, all files are all included in slow I/O
  // simulation.
  SimulatedHybridFileSystem(const std::shared_ptr<FileSystem>& base,
                            const std::string& metadata_file_name,
                            int throughput_multiplier, bool is_full_fs_warm,
                            SimulatedStorageModelOptions model_options = {});

  ~SimulatedHybridFileSystem() override;

 public:
  static const char* kClassName() { return "SimulatedHybridFileSystem"; }

  IOStatus NewRandomAccessFile(const std::string& fname,
                               const FileOptions& file_opts,
                               std::unique_ptr<FSRandomAccessFile>* result,
                               IODebugContext* dbg) override;
  IOStatus NewSequentialFile(const std::string& fname,
                             const FileOptions& file_opts,
                             std::unique_ptr<FSSequentialFile>* result,
                             IODebugContext* dbg) override;
  IOStatus NewDirectory(const std::string& name, const IOOptions& options,
                        std::unique_ptr<FSDirectory>* result,
                        IODebugContext* dbg) override;
  IOStatus NewWritableFile(const std::string& fname,
                           const FileOptions& file_opts,
                           std::unique_ptr<FSWritableFile>* result,
                           IODebugContext* dbg) override;
  IOStatus ReopenWritableFile(const std::string& fname,
                              const FileOptions& file_opts,
                              std::unique_ptr<FSWritableFile>* result,
                              IODebugContext* dbg) override;
  IOStatus ReuseWritableFile(const std::string& fname,
                             const std::string& old_fname,
                             const FileOptions& file_opts,
                             std::unique_ptr<FSWritableFile>* result,
                             IODebugContext* dbg) override;
  IOStatus FileExists(const std::string& fname, const IOOptions& options,
                      IODebugContext* dbg) override;
  IOStatus GetChildren(const std::string& dir, const IOOptions& options,
                       std::vector<std::string>* result,
                       IODebugContext* dbg) override;
  IOStatus GetChildrenFileAttributes(
      const std::string& dir, const IOOptions& options,
      std::vector<FileAttributes>* result, IODebugContext* dbg) override;
  IOStatus LockFile(const std::string& fname, const IOOptions& options,
                    FileLock** lock, IODebugContext* dbg) override;
  IOStatus UnlockFile(FileLock* lock, const IOOptions& options,
                      IODebugContext* dbg) override;
  IOStatus CreateDir(const std::string& dirname, const IOOptions& options,
                     IODebugContext* dbg) override;
  IOStatus CreateDirIfMissing(const std::string& dirname,
                              const IOOptions& options,
                              IODebugContext* dbg) override;
  IOStatus DeleteDir(const std::string& dirname, const IOOptions& options,
                     IODebugContext* dbg) override;
  IOStatus IsDirectory(const std::string& path, const IOOptions& options,
                       bool* is_dir, IODebugContext* dbg) override;
  IOStatus GetFileSize(const std::string& fname, const IOOptions& options,
                       uint64_t* size, IODebugContext* dbg) override;
  IOStatus RenameFile(const std::string& src, const std::string& target,
                      const IOOptions& options, IODebugContext* dbg) override;
  IOStatus NewLogger(const std::string& fname, const IOOptions& options,
                     std::shared_ptr<Logger>* result,
                     IODebugContext* dbg) override;
  IOStatus DeleteFile(const std::string& fname, const IOOptions& options,
                      IODebugContext* dbg) override;

  const char* Name() const override { return kClassName(); }

  // Best-effort: align monitor epoch with benchmark stage start.
  // Calling this resets internal window/stage aggregations.
  void SetMonitorStartTimeMicros(uint64_t start_us);

 private:
  // Limit 100 requests per second. Rate limiter is designed to byte but
  // we use it as fixed bytes is one request.
  std::shared_ptr<RateLimiter> rate_limiter_;
  std::mutex mutex_;
  std::unordered_map<FileLock*, FileLock*> lock_map_;
  std::unordered_set<std::string> warm_file_set_;
  std::string metadata_file_name_;
  std::string name_;
  bool is_full_fs_warm_;
  SimulatedStorageModelOptions model_options_;
  std::shared_ptr<SimulatedStorageModelStats> stats_;
  std::shared_ptr<SimulatedFsLatencyMonitor> latency_monitor_;

  bool ShouldSimulatePath(const std::string& fname) const;
  bool IsTmpfsRedirectEnabled() const;
  std::string NormalizeTmpfsRoot(std::string root) const;
  std::string ToTmpfsPath(const std::string& fname) const;
  bool IsUnderTmpfsRoot(const std::string& fname) const;
  bool TmpfsFileExists(const std::string& fname) const;
  bool ShouldPersistToTmpfs(const std::string& fname) const;
  std::string ResolveReadPath(const std::string& fname) const;
  std::string ResolveWritePath(const std::string& fname) const;
  IOStatus EnsureParentDir(const std::string& path, IODebugContext* dbg) const;
  IOStatus EnsureDirRecursive(const std::string& dir,
                              IODebugContext* dbg) const;
  IOStatus CopyFileContents(const std::string& src, const std::string& dst,
                            IODebugContext* dbg) const;
  void MaybeWriteModelStats() const;
};

// Simulated random access file that can control IOPs and latency to simulate
// specific storage media
class SimulatedHybridRaf : public FSRandomAccessFileOwnerWrapper {
 public:
  SimulatedHybridRaf(std::unique_ptr<FSRandomAccessFile>&& t,
                     std::shared_ptr<RateLimiter> rate_limiter,
                     std::string file_name,
                     bool is_tmpfs,
                     bool should_simulate,
                     const SimulatedStorageModelOptions& model_options,
                     std::shared_ptr<SimulatedStorageModelStats> stats,
                     std::shared_ptr<SimulatedFsLatencyMonitor> monitor)
      : FSRandomAccessFileOwnerWrapper(std::move(t)),
        rate_limiter_(rate_limiter),
        file_name_(std::move(file_name)),
        is_tmpfs_(is_tmpfs),
        should_simulate_(should_simulate),
        model_options_(model_options),
        stats_(std::move(stats)),
        monitor_(std::move(monitor)) {}

  ~SimulatedHybridRaf() override {}

  IOStatus Read(uint64_t offset, size_t n, const IOOptions& options,
                Slice* result, char* scratch,
                IODebugContext* dbg) const override;

  IOStatus MultiRead(FSReadRequest* reqs, size_t num_reqs,
                     const IOOptions& options, IODebugContext* dbg) override;

  IOStatus Prefetch(uint64_t offset, size_t n, const IOOptions& options,
                    IODebugContext* dbg) override;

 private:
  std::shared_ptr<RateLimiter> rate_limiter_;
  std::string file_name_;
  bool is_tmpfs_;
  bool should_simulate_;
  SimulatedStorageModelOptions model_options_;
  std::shared_ptr<SimulatedStorageModelStats> stats_;
  std::shared_ptr<SimulatedFsLatencyMonitor> monitor_;

  void SimulateIOWait(uint64_t offset, uint64_t logical_bytes) const;
};

class SimulatedWritableFile : public FSWritableFileWrapper {
 public:
  SimulatedWritableFile(std::unique_ptr<FSWritableFile>&& t,
                        std::shared_ptr<RateLimiter> rate_limiter,
                        std::string file_name,
                        const SimulatedStorageModelOptions& model_options,
                        std::shared_ptr<SimulatedStorageModelStats> stats)
      : FSWritableFileWrapper(t.get()),
        file_guard_(std::move(t)),
        rate_limiter_(rate_limiter),
        file_name_(std::move(file_name)),
        model_options_(model_options),
        stats_(std::move(stats)) {}
  IOStatus Append(const Slice& data, const IOOptions&,
                  IODebugContext*) override;
  IOStatus Append(const Slice& data, const IOOptions& options,
                  const DataVerificationInfo& verification_info,
                  IODebugContext* dbg) override;
  IOStatus Sync(const IOOptions& options, IODebugContext* dbg) override;
  IOStatus Close(const IOOptions& options, IODebugContext* dbg) override;
  IOStatus PositionedAppend(const Slice& data, uint64_t offset,
                            const IOOptions& options,
                            IODebugContext* dbg) override;
  IOStatus PositionedAppend(const Slice& data, uint64_t offset,
                            const IOOptions& options,
                            const DataVerificationInfo& verification_info,
                            IODebugContext* dbg) override;

 private:
  std::unique_ptr<FSWritableFile> file_guard_;
  std::shared_ptr<RateLimiter> rate_limiter_;
  std::string file_name_;
  SimulatedStorageModelOptions model_options_;
  std::shared_ptr<SimulatedStorageModelStats> stats_;
  size_t unsynced_bytes = 0;
  uint64_t append_file_offset_ = 0;

  void SimulateIOWait(uint64_t logical_bytes, uint64_t media_bytes) const;
  void SimulateXPWrite(uint64_t offset, uint64_t logical_bytes) const;
  void SimulateDimmWrite(uint64_t offset, uint64_t logical_bytes) const;
};
}  // namespace ROCKSDB_NAMESPACE
