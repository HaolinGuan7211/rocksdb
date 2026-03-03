//  Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "tools/nvm_fs/simulated_hybrid_file_system.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <deque>
#include <fstream>
#include <iostream>
#include <list>
#include <limits>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <vector>

#include "rocksdb/rate_limiter.h"
#include "util/stop_watch.h"

namespace ROCKSDB_NAMESPACE {

const int64_t kUsPerSec = 1000000;
const int64_t kDummyBytesPerUs = 1024;

thread_local uint64_t g_simulated_fs_injected_delay_ns = 0;
thread_local uint64_t g_simulated_fs_base_read_ns = 0;
thread_local uint64_t g_simulated_fs_actual_injected_wait_ns = 0;
thread_local uint64_t g_simulated_fs_injected_wait_overshoot_ns = 0;
thread_local uint64_t g_simulated_fs_read_wall_ns = 0;

uint64_t GetSimulatedFsInjectedDelayNsForCurrentThread() {
  return g_simulated_fs_injected_delay_ns;
}

uint64_t GetSimulatedFsBaseReadNsForCurrentThread() {
  return g_simulated_fs_base_read_ns;
}

uint64_t GetSimulatedFsActualInjectedWaitNsForCurrentThread() {
  return g_simulated_fs_actual_injected_wait_ns;
}

uint64_t GetSimulatedFsReadWallNsForCurrentThread() {
  return g_simulated_fs_read_wall_ns;
}

uint64_t GetSimulatedFsInjectedWaitOvershootNsForCurrentThread() {
  return g_simulated_fs_injected_wait_overshoot_ns;
}

namespace {
// From bytes to read/write, calculate service time needed by an HDD.
// This is used to simulate latency from HDD.
int CalculateServeTimeUs(size_t bytes) {
  return 12200 + static_cast<int>(static_cast<double>(bytes) * 0.005215);
}

// There is a bug in rater limiter that would crash with small requests
// Hack to get it around.
void RateLimiterRequest(RateLimiter* rater_limiter, int64_t amount) {
  int64_t left = amount * kDummyBytesPerUs;
  const int64_t kMaxToRequest = kDummyBytesPerUs * kUsPerSec / 1024;
  while (left > 0) {
    int64_t to_request = std::min(kMaxToRequest, left);
    rater_limiter->Request(to_request, Env::IOPriority::IO_LOW, nullptr);
    left -= to_request;
  }
}

uint64_t CeilDiv(uint64_t a, uint64_t b) {
  return (a + b - 1) / b;
}

uint64_t GetXpServiceNsForLines(const SimulatedStorageModelOptions& options,
                                uint64_t lines, bool is_write);

uint64_t GetXpServiceNs(const SimulatedStorageModelOptions& options,
                        uint64_t bytes, bool is_write) {
  const uint64_t service_bytes =
      std::max<uint64_t>(1, options.xp_service_bytes);
  const uint64_t lines = std::max<uint64_t>(1, CeilDiv(bytes, service_bytes));
  return GetXpServiceNsForLines(options, lines, is_write);
}

uint64_t GetXpServiceNsForLines(const SimulatedStorageModelOptions& options,
                                uint64_t lines, bool is_write) {
  lines = std::max<uint64_t>(1, lines);
  if (lines <= 1) {
    return std::max<uint64_t>(1, options.xp_latency_ns);
  }
  const uint64_t line_parallelism =
      std::max<uint64_t>(1, is_write ? options.xp_write_line_parallelism
                                     : options.xp_read_line_parallelism);
  const uint64_t total_ns = lines * options.xp_latency_ns;
  return std::max<uint64_t>(1, CeilDiv(total_ns, line_parallelism));
}

uint64_t GetLineCount(uint64_t offset, uint64_t logical_bytes,
                      uint64_t line_bytes) {
  if (logical_bytes == 0) {
    return 0;
  }
  const uint64_t offset_in_line = offset % line_bytes;
  const uint64_t max_add = std::numeric_limits<uint64_t>::max() - offset_in_line;
  const uint64_t covered = std::min<uint64_t>(logical_bytes, max_add);
  return std::max<uint64_t>(1, CeilDiv(offset_in_line + covered, line_bytes));
}

uint64_t GetDramReadBaseNs(const SimulatedStorageModelOptions& options,
                           bool is_sequential) {
  return is_sequential ? options.dram_read_seq_ns : options.dram_read_rand_ns;
}

uint64_t GetMediaBytes(const SimulatedStorageModelOptions& options,
                       uint64_t logical_bytes) {
  if (!options.use_xp_model && !options.use_dimm_model) {
    return logical_bytes;
  }
  const uint64_t line_bytes = std::max<uint64_t>(1, options.xp_line_bytes);
  return CeilDiv(logical_bytes, line_bytes) * line_bytes;
}

double ClampDouble(double v, double lo, double hi) {
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

uint64_t CeilDivDoubleToU64(double num, double denom) {
  if (!(denom > 0.0)) {
    return 0;
  }
  const double raw = std::ceil(num / denom);
  if (!(raw > 0.0)) {
    return 0;
  }
  if (raw >= static_cast<double>(std::numeric_limits<uint64_t>::max())) {
    return std::numeric_limits<uint64_t>::max();
  }
  return static_cast<uint64_t>(raw);
}

double GetDimmEffectiveBandwidthGbps(const SimulatedStorageModelOptions& options,
                                     bool is_write, bool is_random) {
  double bw = is_write ? options.dimm_seq_write_bw_gbps
                       : options.dimm_seq_read_bw_gbps;
  if (is_random) {
    bw *= ClampDouble(options.dimm_rand_bw_scale, 0.0, 1.0);
  }
  // 1 GB/s (decimal) == 1 byte/ns. Keep a non-zero floor.
  return std::max(1e-9, bw);
}

uint64_t GetDimmEffectiveMediaBytes(const SimulatedStorageModelOptions& options,
                                    uint64_t logical_bytes, bool is_random) {
  if (logical_bytes == 0) {
    return 0;
  }
  uint64_t media_bytes = GetMediaBytes(options, logical_bytes);
  const uint64_t line_bytes = std::max<uint64_t>(1, options.xp_line_bytes);
  if (is_random && logical_bytes < line_bytes) {
    const double amp = std::max(1.0, options.dimm_sub_line_random_media_amp);
    const uint64_t amplified =
        CeilDivDoubleToU64(static_cast<double>(logical_bytes) * amp, 1.0);
    media_bytes = std::max<uint64_t>(media_bytes, amplified);
  }
  return media_bytes;
}

uint64_t GetDimmTransferTimeNs(uint64_t media_bytes, double bw_gbps) {
  if (media_bytes == 0) {
    return 0;
  }
  // bw_gbps is bytes/ns when using decimal GB/s.
  const double bw = std::max(1e-9, bw_gbps);
  return std::max<uint64_t>(1,
                            CeilDivDoubleToU64(static_cast<double>(media_bytes),
                                               bw));
}

void BusySpinForNanoseconds(uint64_t delay_ns) {
  if (delay_ns == 0) {
    return;
  }
  const auto clock = Env::Default()->GetSystemClock();
  const uint64_t start = clock->NowNanos();
  while (clock->NowNanos() - start < delay_ns) {
  }
}

void WaitForNanoseconds(uint64_t delay_ns, bool busy_wait) {
  if (delay_ns == 0) {
    return;
  }
  const uint64_t intended_delay_ns = delay_ns;
  g_simulated_fs_injected_delay_ns += delay_ns;
  const auto clock = Env::Default()->GetSystemClock();
  const uint64_t wait_start_ns = clock->NowNanos();
  if (busy_wait) {
    BusySpinForNanoseconds(delay_ns);
  } else {
    if (delay_ns >= 1000) {
      Env::Default()->SleepForMicroseconds(static_cast<int>(delay_ns / 1000));
      delay_ns %= 1000;
      if (delay_ns != 0) {
        BusySpinForNanoseconds(delay_ns);
      }
    } else {
      BusySpinForNanoseconds(delay_ns);
    }
  }
  const uint64_t wait_end_ns = clock->NowNanos();
  if (wait_end_ns >= wait_start_ns) {
    const uint64_t waited_ns = wait_end_ns - wait_start_ns;
    g_simulated_fs_actual_injected_wait_ns += waited_ns;
    if (waited_ns > intended_delay_ns) {
      g_simulated_fs_injected_wait_overshoot_ns +=
          (waited_ns - intended_delay_ns);
    }
  }
}

// When bypassing base IO (virtual-time mode), we still want to record the
// intended injected delay so Gate2/Gate3 attribution remains meaningful.
// This helper accounts "expected" delay without sleeping/spinning.
void AccountInjectedDelayOnly(uint64_t delay_ns) {
  if (delay_ns == 0) {
    return;
  }
  g_simulated_fs_injected_delay_ns += delay_ns;
}

uint64_t NowNanos() { return Env::Default()->GetSystemClock()->NowNanos(); }
uint64_t NowMicros() { return Env::Default()->GetSystemClock()->NowMicros(); }

thread_local bool g_xp_forced_stream_tag_valid = false;
thread_local uint64_t g_xp_forced_stream_tag = 0;

uint64_t GetThreadStreamTag() {
  if (g_xp_forced_stream_tag_valid) {
    return g_xp_forced_stream_tag;
  }
  thread_local uint64_t tag =
      static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
  return tag;
}

struct XPWriteQueueEntry {
  std::string file_name;
  uint64_t start = 0;
  uint64_t end = 0;
  uint64_t finish_ns = 0;
  uint64_t counted_media_bytes = 0;
  uint64_t counted_service_ns = 0;
  uint64_t issue_start_ns = 0;
  uint32_t server_idx = 0;
};

struct XPReadStreamState {
  bool has_last = false;
  uint32_t last_file_id = 0;
  uint64_t last_end = 0;
};

// XP stream keying:
// The simulation tracks per-stream sequentiality to decide DRAM latency and
// random bandwidth scaling. A previous implementation tracked sequentiality per
// (file, stream) via a hash map keyed by string/packed keys. For large DBs this
// can create huge maps (file_count * stream_count) and lead to allocator/rehash
// stalls under the global simulator lock, producing synchronous tail noise.
//
// We instead map stream tags to small integer ids and maintain *one* stream
// state per stream id, tracking the last (file_id, last_end) tuple. This is a
// good approximation for our workloads while keeping the hot path allocation-
// free and bounded.
using XPStreamId = uint32_t;

struct XPBufferLineKey {
  uint32_t file_id = 0;
  uint64_t line_idx = 0;
  bool operator==(const XPBufferLineKey& rhs) const {
    return line_idx == rhs.line_idx && file_id == rhs.file_id;
  }
};

struct XPBufferLineKeyHash {
  size_t operator()(const XPBufferLineKey& k) const {
    const size_t h1 = std::hash<uint32_t>{}(k.file_id);
    const size_t h2 = std::hash<uint64_t>{}(k.line_idx);
    return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
  }
};

struct XPControllerState {
  std::vector<uint64_t> wpq_server_available_ns;
  std::vector<uint64_t> rpq_server_available_ns;
  std::deque<XPWriteQueueEntry> wpq;
  std::deque<uint64_t> rpq_finish_times;

  // XPBuffer: a small fixed-capacity LRU of recently accessed "lines".
  //
  // This is on the *simulation* fast path (protected by g_xp_mu). For small
  // capacities (e.g. 16KB buffer / 256B line = 64 lines), a vector-based LRU
  // avoids hashing and allocations while preserving exact LRU semantics.
  bool xp_use_small_lru = false;
  uint64_t xp_small_capacity_lines = 0;
  std::vector<XPBufferLineKey> xp_small_lru;

  // Fallback for large capacities.
  std::list<XPBufferLineKey> xp_buffer_lru;
  std::unordered_map<XPBufferLineKey, std::list<XPBufferLineKey>::iterator,
                     XPBufferLineKeyHash>
      xp_buffer_index;
  bool xp_buffer_reserved = false;

  std::unordered_map<std::string, uint32_t> file_name_to_id;
  uint32_t next_file_id = 1;
  uint32_t wpq_rr_cursor = 0;
  uint32_t rpq_rr_cursor = 0;
  std::unordered_map<uint32_t, uint32_t> write_stream_server_by_file_id;
  std::unordered_map<uint64_t, XPStreamId> stream_tag_to_id;
  XPStreamId next_stream_id = 1;
  std::vector<XPReadStreamState> read_stream_states_by_id;
};

std::mutex g_xp_mu;
std::unordered_map<uintptr_t, XPControllerState> g_xp_states;
struct DimmStreamState {
  bool has_last = false;
  uint32_t last_file_id = 0;
  uint64_t last_end = 0;
};

struct DimmControllerState {
  std::vector<uint64_t> read_server_available_ns;
  std::vector<uint64_t> write_server_available_ns;
  std::deque<uint64_t> read_finish_times;
  std::deque<uint64_t> write_finish_times;
  uint32_t read_rr_cursor = 0;
  uint32_t write_rr_cursor = 0;
  std::unordered_map<std::string, uint32_t> file_name_to_id;
  uint32_t next_file_id = 1;
  std::unordered_map<uint64_t, uint32_t> stream_tag_to_id;
  uint32_t next_stream_id = 1;
  std::vector<DimmStreamState> read_stream_states_by_id;
  std::vector<DimmStreamState> write_stream_states_by_id;
};

std::mutex g_dimm_mu;
std::unordered_map<uintptr_t, DimmControllerState> g_dimm_states;
}  // namespace

enum class SimFsMonitorOp : uint8_t { kRead = 0, kOpen = 1, kPrefetch = 2 };

struct SimFsMonitorCounters {
  std::atomic<uint64_t> ops{0};
  std::atomic<uint64_t> bytes{0};
  std::atomic<uint64_t> max_us{0};
  std::atomic<uint64_t> tmpfs_ops{0};
  std::atomic<uint64_t> tmpfs_bytes{0};
  std::atomic<uint64_t> base_ops{0};
  std::atomic<uint64_t> base_bytes{0};
};

class SimulatedFsLatencyMonitor {
 public:
  explicit SimulatedFsLatencyMonitor(const SimulatedStorageModelOptions& options)
      : enabled_(options.monitor_enable),
        window_us_(std::max<uint64_t>(1, options.monitor_window_us)),
        stage_us_(options.monitor_stage_seconds > 0
                      ? options.monitor_stage_seconds * 1000000ULL
                      : 0),
        max_read_(options.monitor_max_read),
        max_open_(options.monitor_max_open),
        max_prefetch_(options.monitor_max_prefetch),
        window_csv_(options.monitor_window_csv),
        stage_csv_(options.monitor_stage_csv) {
    if (!enabled_) {
      return;
    }
    if (window_csv_.empty() && stage_csv_.empty()) {
      enabled_ = false;
      return;
    }
    start_us_.store(Env::Default()->GetSystemClock()->NowMicros());
    OpenOutputs();
    thread_ = std::thread([this]() { ThreadBody(); });
  }

  ~SimulatedFsLatencyMonitor() { Stop(); }

  bool Enabled() const { return enabled_; }

  void SetStartTimeMicros(uint64_t start_us) {
    if (!enabled_) {
      return;
    }
    start_us_.store(start_us);
    // Reset counters to avoid mixing stages across benchmark phases.
    ResetCounters();
    Notify();
  }

  void Record(SimFsMonitorOp op, bool is_tmpfs, uint64_t bytes,
              uint64_t latency_us) {
    if (!enabled_) {
      return;
    }
    if (latency_us == 0) {
      latency_us = 1;
    }
    switch (op) {
      case SimFsMonitorOp::kRead:
        if (max_read_) {
          AtomicMax(&window_read_.max_us, latency_us);
          AtomicMax(&stage_read_.max_us, latency_us);
        }
        window_read_.ops.fetch_add(1);
        window_read_.bytes.fetch_add(bytes);
        stage_read_.ops.fetch_add(1);
        stage_read_.bytes.fetch_add(bytes);
        if (is_tmpfs) {
          window_read_.tmpfs_ops.fetch_add(1);
          window_read_.tmpfs_bytes.fetch_add(bytes);
          stage_read_.tmpfs_ops.fetch_add(1);
          stage_read_.tmpfs_bytes.fetch_add(bytes);
        } else {
          window_read_.base_ops.fetch_add(1);
          window_read_.base_bytes.fetch_add(bytes);
          stage_read_.base_ops.fetch_add(1);
          stage_read_.base_bytes.fetch_add(bytes);
        }
        return;
      case SimFsMonitorOp::kOpen:
        if (max_open_) {
          AtomicMax(&window_open_.max_us, latency_us);
          AtomicMax(&stage_open_.max_us, latency_us);
        }
        window_open_.ops.fetch_add(1);
        stage_open_.ops.fetch_add(1);
        if (is_tmpfs) {
          window_open_.tmpfs_ops.fetch_add(1);
          stage_open_.tmpfs_ops.fetch_add(1);
        } else {
          window_open_.base_ops.fetch_add(1);
          stage_open_.base_ops.fetch_add(1);
        }
        return;
      case SimFsMonitorOp::kPrefetch:
        if (max_prefetch_) {
          AtomicMax(&window_prefetch_.max_us, latency_us);
          AtomicMax(&stage_prefetch_.max_us, latency_us);
        }
        window_prefetch_.ops.fetch_add(1);
        window_prefetch_.bytes.fetch_add(bytes);
        stage_prefetch_.ops.fetch_add(1);
        stage_prefetch_.bytes.fetch_add(bytes);
        if (is_tmpfs) {
          window_prefetch_.tmpfs_ops.fetch_add(1);
          window_prefetch_.tmpfs_bytes.fetch_add(bytes);
          stage_prefetch_.tmpfs_ops.fetch_add(1);
          stage_prefetch_.tmpfs_bytes.fetch_add(bytes);
        } else {
          window_prefetch_.base_ops.fetch_add(1);
          window_prefetch_.base_bytes.fetch_add(bytes);
          stage_prefetch_.base_ops.fetch_add(1);
          stage_prefetch_.base_bytes.fetch_add(bytes);
        }
        return;
    }
  }

  void Stop() {
    if (!enabled_) {
      return;
    }
    {
      std::lock_guard<std::mutex> lk(cv_mu_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
    // Final flush to avoid losing the last partial window/stage.
    FlushWindow(/*force=*/true);
    FlushStage(/*force=*/true);
    if (window_out_.is_open()) {
      window_out_.flush();
      window_out_.close();
    }
    if (stage_out_.is_open()) {
      stage_out_.flush();
      stage_out_.close();
    }
    enabled_ = false;
  }

 private:
  void Notify() {
    std::lock_guard<std::mutex> lk(cv_mu_);
    cv_.notify_all();
  }

  void ResetCounters() {
    window_read_.ops.store(0);
    window_read_.bytes.store(0);
    window_read_.max_us.store(0);
    window_read_.tmpfs_ops.store(0);
    window_read_.tmpfs_bytes.store(0);
    window_read_.base_ops.store(0);
    window_read_.base_bytes.store(0);

    window_open_.ops.store(0);
    window_open_.bytes.store(0);
    window_open_.max_us.store(0);
    window_open_.tmpfs_ops.store(0);
    window_open_.tmpfs_bytes.store(0);
    window_open_.base_ops.store(0);
    window_open_.base_bytes.store(0);

    window_prefetch_.ops.store(0);
    window_prefetch_.bytes.store(0);
    window_prefetch_.max_us.store(0);
    window_prefetch_.tmpfs_ops.store(0);
    window_prefetch_.tmpfs_bytes.store(0);
    window_prefetch_.base_ops.store(0);
    window_prefetch_.base_bytes.store(0);

    stage_read_.ops.store(0);
    stage_read_.bytes.store(0);
    stage_read_.max_us.store(0);
    stage_read_.tmpfs_ops.store(0);
    stage_read_.tmpfs_bytes.store(0);
    stage_read_.base_ops.store(0);
    stage_read_.base_bytes.store(0);

    stage_open_.ops.store(0);
    stage_open_.bytes.store(0);
    stage_open_.max_us.store(0);
    stage_open_.tmpfs_ops.store(0);
    stage_open_.tmpfs_bytes.store(0);
    stage_open_.base_ops.store(0);
    stage_open_.base_bytes.store(0);

    stage_prefetch_.ops.store(0);
    stage_prefetch_.bytes.store(0);
    stage_prefetch_.max_us.store(0);
    stage_prefetch_.tmpfs_ops.store(0);
    stage_prefetch_.tmpfs_bytes.store(0);
    stage_prefetch_.base_ops.store(0);
    stage_prefetch_.base_bytes.store(0);

    last_flushed_window_id_ = -1;
    last_flushed_stage_id_ = -1;
  }

  void OpenOutputs() {
    if (!window_csv_.empty()) {
      window_out_.open(window_csv_, std::ofstream::out | std::ofstream::trunc);
      if (window_out_.good()) {
        window_out_
            << "window_id,window_start_us,window_end_us,"
               "read_ops,read_bytes,tmpfs_read_ops,tmpfs_read_bytes,base_read_ops,base_read_bytes,max_read_us,"
               "prefetch_ops,prefetch_bytes,tmpfs_prefetch_ops,tmpfs_prefetch_bytes,base_prefetch_ops,base_prefetch_bytes,max_prefetch_us,"
               "open_ops,tmpfs_open_ops,base_open_ops,max_open_us\n";
      }
    }
    if (!stage_csv_.empty()) {
      stage_out_.open(stage_csv_, std::ofstream::out | std::ofstream::trunc);
      if (stage_out_.good()) {
        stage_out_
            << "stage_id,stage_start_us,stage_end_us,"
               "read_ops,read_bytes,tmpfs_read_ops,tmpfs_read_bytes,base_read_ops,base_read_bytes,max_read_us,"
               "prefetch_ops,prefetch_bytes,tmpfs_prefetch_ops,tmpfs_prefetch_bytes,base_prefetch_ops,base_prefetch_bytes,max_prefetch_us,"
               "open_ops,tmpfs_open_ops,base_open_ops,max_open_us\n";
      }
    }
  }

  static uint64_t ExchangeMax(std::atomic<uint64_t>* v) {
    return v ? v->exchange(0) : 0;
  }

  static void AtomicMax(std::atomic<uint64_t>* dst, uint64_t value) {
    if (dst == nullptr) {
      return;
    }
    uint64_t prev = dst->load(std::memory_order_relaxed);
    while (prev < value &&
           !dst->compare_exchange_weak(prev, value, std::memory_order_relaxed)) {
    }
  }

  void FlushWindow(bool force) {
    if (!window_out_.is_open()) {
      return;
    }
    const uint64_t start_us = start_us_.load();
    const uint64_t now_us = Env::Default()->GetSystemClock()->NowMicros();
    const uint64_t elapsed_us = now_us > start_us ? now_us - start_us : 0;
    const uint64_t windows_started = elapsed_us / window_us_;
    int64_t target = static_cast<int64_t>(windows_started);
    if (!force) {
      if (windows_started == 0) {
        return;
      }
      target = static_cast<int64_t>(windows_started - 1);
    }
    if (target <= last_flushed_window_id_) {
      return;
    }

    const uint64_t w_start = start_us + static_cast<uint64_t>(target) * window_us_;
    const uint64_t w_end = w_start + window_us_;

    const uint64_t read_ops = window_read_.ops.exchange(0);
    const uint64_t read_bytes = window_read_.bytes.exchange(0);
    const uint64_t tmpfs_read_ops = window_read_.tmpfs_ops.exchange(0);
    const uint64_t tmpfs_read_bytes = window_read_.tmpfs_bytes.exchange(0);
    const uint64_t base_read_ops = window_read_.base_ops.exchange(0);
    const uint64_t base_read_bytes = window_read_.base_bytes.exchange(0);
    const uint64_t max_read_us = ExchangeMax(&window_read_.max_us);

    const uint64_t prefetch_ops = window_prefetch_.ops.exchange(0);
    const uint64_t prefetch_bytes = window_prefetch_.bytes.exchange(0);
    const uint64_t tmpfs_prefetch_ops = window_prefetch_.tmpfs_ops.exchange(0);
    const uint64_t tmpfs_prefetch_bytes = window_prefetch_.tmpfs_bytes.exchange(0);
    const uint64_t base_prefetch_ops = window_prefetch_.base_ops.exchange(0);
    const uint64_t base_prefetch_bytes = window_prefetch_.base_bytes.exchange(0);
    const uint64_t max_prefetch_us = ExchangeMax(&window_prefetch_.max_us);

    const uint64_t open_ops = window_open_.ops.exchange(0);
    (void)window_open_.bytes.exchange(0);
    const uint64_t tmpfs_open_ops = window_open_.tmpfs_ops.exchange(0);
    const uint64_t base_open_ops = window_open_.base_ops.exchange(0);
    const uint64_t max_open_us = ExchangeMax(&window_open_.max_us);

    window_out_ << target << "," << w_start << "," << w_end << "," << read_ops
                << "," << read_bytes << "," << tmpfs_read_ops << ","
                << tmpfs_read_bytes << "," << base_read_ops << ","
                << base_read_bytes << "," << max_read_us << ","
                << prefetch_ops << "," << prefetch_bytes << ","
                << tmpfs_prefetch_ops << "," << tmpfs_prefetch_bytes << ","
                << base_prefetch_ops << "," << base_prefetch_bytes << ","
                << max_prefetch_us << "," << open_ops << "," << tmpfs_open_ops
                << "," << base_open_ops << "," << max_open_us << "\n";
    last_flushed_window_id_ = target;
    window_out_.flush();
  }

  void FlushStage(bool force) {
    if (stage_us_ == 0 || !stage_out_.is_open()) {
      return;
    }
    const uint64_t start_us = start_us_.load();
    const uint64_t now_us = Env::Default()->GetSystemClock()->NowMicros();
    const uint64_t elapsed_us = now_us > start_us ? now_us - start_us : 0;
    const uint64_t stages_started = elapsed_us / stage_us_;
    int64_t target = static_cast<int64_t>(stages_started);
    if (!force) {
      if (stages_started == 0) {
        return;
      }
      target = static_cast<int64_t>(stages_started - 1);
    }
    if (target <= last_flushed_stage_id_) {
      return;
    }

    const uint64_t s_start = start_us + static_cast<uint64_t>(target) * stage_us_;
    const uint64_t s_end = s_start + stage_us_;

    const uint64_t read_ops = stage_read_.ops.exchange(0);
    const uint64_t read_bytes = stage_read_.bytes.exchange(0);
    const uint64_t tmpfs_read_ops = stage_read_.tmpfs_ops.exchange(0);
    const uint64_t tmpfs_read_bytes = stage_read_.tmpfs_bytes.exchange(0);
    const uint64_t base_read_ops = stage_read_.base_ops.exchange(0);
    const uint64_t base_read_bytes = stage_read_.base_bytes.exchange(0);
    const uint64_t max_read_us = ExchangeMax(&stage_read_.max_us);

    const uint64_t prefetch_ops = stage_prefetch_.ops.exchange(0);
    const uint64_t prefetch_bytes = stage_prefetch_.bytes.exchange(0);
    const uint64_t tmpfs_prefetch_ops = stage_prefetch_.tmpfs_ops.exchange(0);
    const uint64_t tmpfs_prefetch_bytes = stage_prefetch_.tmpfs_bytes.exchange(0);
    const uint64_t base_prefetch_ops = stage_prefetch_.base_ops.exchange(0);
    const uint64_t base_prefetch_bytes = stage_prefetch_.base_bytes.exchange(0);
    const uint64_t max_prefetch_us = ExchangeMax(&stage_prefetch_.max_us);

    const uint64_t open_ops = stage_open_.ops.exchange(0);
    (void)stage_open_.bytes.exchange(0);
    const uint64_t tmpfs_open_ops = stage_open_.tmpfs_ops.exchange(0);
    const uint64_t base_open_ops = stage_open_.base_ops.exchange(0);
    const uint64_t max_open_us = ExchangeMax(&stage_open_.max_us);

    stage_out_ << target << "," << s_start << "," << s_end << "," << read_ops
               << "," << read_bytes << "," << tmpfs_read_ops << ","
               << tmpfs_read_bytes << "," << base_read_ops << ","
               << base_read_bytes << "," << max_read_us << ","
               << prefetch_ops << "," << prefetch_bytes << ","
               << tmpfs_prefetch_ops << "," << tmpfs_prefetch_bytes << ","
               << base_prefetch_ops << "," << base_prefetch_bytes << ","
               << max_prefetch_us << "," << open_ops << "," << tmpfs_open_ops
               << "," << base_open_ops << "," << max_open_us << "\n";
    last_flushed_stage_id_ = target;
    stage_out_.flush();
  }

  void ThreadBody() {
    while (true) {
      {
        std::unique_lock<std::mutex> lk(cv_mu_);
        cv_.wait_for(lk, std::chrono::microseconds(window_us_), [this]() {
          return stop_;
        });
        if (stop_) {
          return;
        }
      }
      FlushWindow(/*force=*/false);
      FlushStage(/*force=*/false);
    }
  }

  bool enabled_{false};
  const uint64_t window_us_;
  const uint64_t stage_us_;
  const bool max_read_;
  const bool max_open_;
  const bool max_prefetch_;
  const std::string window_csv_;
  const std::string stage_csv_;

  std::atomic<uint64_t> start_us_{0};
  SimFsMonitorCounters window_read_;
  SimFsMonitorCounters window_open_;
  SimFsMonitorCounters window_prefetch_;
  SimFsMonitorCounters stage_read_;
  SimFsMonitorCounters stage_open_;
  SimFsMonitorCounters stage_prefetch_;
  int64_t last_flushed_window_id_{-1};
  int64_t last_flushed_stage_id_{-1};

  std::ofstream window_out_;
  std::ofstream stage_out_;

  std::mutex cv_mu_;
  std::condition_variable cv_;
  bool stop_{false};
  std::thread thread_;
};

namespace {
std::mutex g_xp_file_levels_mu;
std::unordered_map<std::string, int> g_xp_file_levels;
struct VirtualNowKey {
  uintptr_t instance_id = 0;
  uint64_t stream_tag = 0;
  bool operator==(const VirtualNowKey& rhs) const {
    return instance_id == rhs.instance_id && stream_tag == rhs.stream_tag;
  }
};

struct VirtualNowKeyHash {
  size_t operator()(const VirtualNowKey& k) const {
    size_t h1 = std::hash<uintptr_t>{}(k.instance_id);
    size_t h2 = std::hash<uint64_t>{}(k.stream_tag);
    return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
  }
};

thread_local std::unordered_map<VirtualNowKey, uint64_t, VirtualNowKeyHash>
    g_xp_thread_virtual_now_ns;

uintptr_t GetInstanceId(
    const std::shared_ptr<SimulatedStorageModelStats>& stats) {
  return reinterpret_cast<uintptr_t>(stats.get());
}

void CleanupXpControllerState(
    const std::shared_ptr<SimulatedStorageModelStats>& stats) {
  if (stats == nullptr) {
    return;
  }
  const uintptr_t instance_id = GetInstanceId(stats);
  std::lock_guard<std::mutex> lk(g_xp_mu);
  g_xp_states.erase(instance_id);
  for (auto it = g_xp_thread_virtual_now_ns.begin();
       it != g_xp_thread_virtual_now_ns.end();) {
    if (it->first.instance_id == instance_id) {
      it = g_xp_thread_virtual_now_ns.erase(it);
    } else {
      ++it;
    }
  }
}

void CleanupDimmControllerState(
    const std::shared_ptr<SimulatedStorageModelStats>& stats) {
  if (stats == nullptr) {
    return;
  }
  const uintptr_t instance_id = GetInstanceId(stats);
  std::lock_guard<std::mutex> lk(g_dimm_mu);
  g_dimm_states.erase(instance_id);
}

bool IsSstFileName(const std::string& fname) {
  static const std::string kSstSuffix = ".sst";
  if (fname.size() < kSstSuffix.size()) {
    return false;
  }
  return fname.compare(fname.size() - kSstSuffix.size(), kSstSuffix.size(),
                       kSstSuffix) == 0;
}

bool LookupSimulatedFsFileLevel(const std::string& file_name, int* level_out) {
  std::lock_guard<std::mutex> lk(g_xp_file_levels_mu);
  auto it = g_xp_file_levels.find(file_name);
  if (it == g_xp_file_levels.end()) {
    return false;
  }
  if (level_out != nullptr) {
    *level_out = it->second;
  }
  return true;
}

uint64_t GetSimNowNs(uintptr_t instance_id, bool bypass_base_io,
                     uint64_t forced_tag_init_stagger_ns) {
  const uint64_t wall_now = NowNanos();
  if (!bypass_base_io) {
    return wall_now;
  }
  const VirtualNowKey key{instance_id, GetThreadStreamTag()};
  auto it = g_xp_thread_virtual_now_ns.find(key);
  if (it == g_xp_thread_virtual_now_ns.end()) {
    uint64_t init_now = wall_now;
    if (g_xp_forced_stream_tag_valid && forced_tag_init_stagger_ns > 0) {
      init_now = g_xp_forced_stream_tag * forced_tag_init_stagger_ns;
    }
    g_xp_thread_virtual_now_ns.emplace(key, init_now);
    return init_now;
  }
  return it->second;
}

void AdvanceSimNowNs(uintptr_t instance_id, bool bypass_base_io,
                     uint64_t ready_ns) {
  if (!bypass_base_io) {
    return;
  }
  const VirtualNowKey key{instance_id, GetThreadStreamTag()};
  uint64_t& thread_now = g_xp_thread_virtual_now_ns[key];
  if (thread_now < ready_ns) {
    thread_now = ready_ns;
  }
}

uint64_t GetSaturatedEnd(uint64_t offset, uint64_t logical_bytes) {
  if (logical_bytes > std::numeric_limits<uint64_t>::max() - offset) {
    return std::numeric_limits<uint64_t>::max();
  }
  return offset + logical_bytes;
}

void UpdateAtomicMinNonZero(std::atomic<uint64_t>* atom, uint64_t value) {
  if (atom == nullptr || value == 0) {
    return;
  }
  uint64_t observed = atom->load();
  while ((observed == 0 || value < observed) &&
         !atom->compare_exchange_weak(observed, value)) {
  }
}

void UpdateAtomicMax(std::atomic<uint64_t>* atom, uint64_t value) {
  if (atom == nullptr) {
    return;
  }
  uint64_t observed = atom->load();
  while (value > observed && !atom->compare_exchange_weak(observed, value)) {
  }
}

void EnsureServerAvailability(std::vector<uint64_t>* server_available,
                              uint64_t parallelism, uint64_t now_ns) {
  if (server_available == nullptr) {
    return;
  }
  const uint64_t size = std::max<uint64_t>(1, parallelism);
  if (server_available->empty()) {
    server_available->assign(size, now_ns);
    return;
  }
  if (server_available->size() < size) {
    server_available->resize(size, now_ns);
  } else if (server_available->size() > size) {
    server_available->resize(size);
  }
}

std::pair<uint32_t, uint64_t> SelectEarliestServer(
    const std::vector<uint64_t>& server_available) {
  uint32_t idx = 0;
  uint64_t earliest = server_available.empty() ? 0 : server_available[0];
  for (uint32_t i = 1; i < server_available.size(); ++i) {
    if (server_available[i] < earliest) {
      earliest = server_available[i];
      idx = i;
    }
  }
  return {idx, earliest};
}

std::pair<uint32_t, uint64_t> SelectEarliestServerWithHint(
    const std::vector<uint64_t>& server_available, uint32_t start_hint) {
  if (server_available.empty()) {
    return {0, 0};
  }
  uint64_t earliest = server_available[0];
  for (size_t i = 1; i < server_available.size(); ++i) {
    earliest = std::min(earliest, server_available[i]);
  }
  const uint32_t n = static_cast<uint32_t>(server_available.size());
  const uint32_t start = start_hint % n;
  for (uint32_t step = 0; step < n; ++step) {
    const uint32_t idx = (start + step) % n;
    if (server_available[idx] == earliest) {
      return {idx, earliest};
    }
  }
  return SelectEarliestServer(server_available);
}

uint64_t MinFinishTime(const std::deque<uint64_t>& finishes) {
  if (finishes.empty()) {
    return 0;
  }
  uint64_t min_finish = finishes.front();
  for (const uint64_t finish : finishes) {
    min_finish = std::min(min_finish, finish);
  }
  return min_finish;
}

uint64_t MinFinishTime(const std::deque<XPWriteQueueEntry>& entries) {
  if (entries.empty()) {
    return 0;
  }
  uint64_t min_finish = entries.front().finish_ns;
  for (const auto& e : entries) {
    min_finish = std::min(min_finish, e.finish_ns);
  }
  return min_finish;
}

void PurgeFinishedWPQ(XPControllerState* state, uint64_t now_ns) {
  if (state == nullptr) {
    return;
  }
  auto new_end =
      std::remove_if(state->wpq.begin(), state->wpq.end(),
                     [now_ns](const XPWriteQueueEntry& e) {
                       return e.finish_ns <= now_ns;
                     });
  state->wpq.erase(new_end, state->wpq.end());
}

void PurgeFinishedRPQ(XPControllerState* state, uint64_t now_ns) {
  if (state == nullptr) {
    return;
  }
  auto new_end = std::remove_if(state->rpq_finish_times.begin(),
                                state->rpq_finish_times.end(),
                                [now_ns](uint64_t finish) {
                                  return finish <= now_ns;
                                });
  state->rpq_finish_times.erase(new_end, state->rpq_finish_times.end());
}

void PurgeFinishedTimes(std::deque<uint64_t>* finishes, uint64_t now_ns) {
  if (finishes == nullptr) {
    return;
  }
  auto new_end =
      std::remove_if(finishes->begin(), finishes->end(),
                     [now_ns](uint64_t finish) { return finish <= now_ns; });
  finishes->erase(new_end, finishes->end());
}

uint64_t GetXpBufferCapacityLines(const SimulatedStorageModelOptions& options) {
  const uint64_t line_bytes = std::max<uint64_t>(1, options.xp_line_bytes);
  return std::max<uint64_t>(1, options.xp_buffer_bytes / line_bytes);
}

constexpr uint64_t kXpSmallMaxLines = 4096;

uint32_t GetXpFileId(XPControllerState* state, const std::string& file_name) {
  if (state == nullptr) {
    return 0;
  }
  auto it = state->file_name_to_id.find(file_name);
  if (it != state->file_name_to_id.end()) {
    return it->second;
  }
  const uint32_t id = state->next_file_id++;
  auto inserted = state->file_name_to_id.emplace(file_name, id);
  return inserted.first->second;
}

XPStreamId GetOrAssignXpStreamId(XPControllerState* state, uint64_t tag) {
  if (state == nullptr) {
    return 0;
  }
  auto it = state->stream_tag_to_id.find(tag);
  if (it != state->stream_tag_to_id.end()) {
    return it->second;
  }
  const XPStreamId id = state->next_stream_id++;
  state->stream_tag_to_id.emplace(tag, id);
  if (state->read_stream_states_by_id.size() <= static_cast<size_t>(id)) {
    state->read_stream_states_by_id.resize(static_cast<size_t>(id) + 1);
  }
  return id;
}

uint32_t GetOrAssignDimmStreamId(DimmControllerState* state, uint64_t tag) {
  if (state == nullptr) {
    return 0;
  }
  auto it = state->stream_tag_to_id.find(tag);
  if (it != state->stream_tag_to_id.end()) {
    return it->second;
  }
  const uint32_t id = state->next_stream_id++;
  state->stream_tag_to_id.emplace(tag, id);
  if (state->read_stream_states_by_id.size() <= static_cast<size_t>(id)) {
    state->read_stream_states_by_id.resize(static_cast<size_t>(id) + 1);
  }
  if (state->write_stream_states_by_id.size() <= static_cast<size_t>(id)) {
    state->write_stream_states_by_id.resize(static_cast<size_t>(id) + 1);
  }
  return id;
}

uint32_t GetDimmFileId(DimmControllerState* state, const std::string& file_name) {
  if (state == nullptr) {
    return 0;
  }
  auto it = state->file_name_to_id.find(file_name);
  if (it != state->file_name_to_id.end()) {
    return it->second;
  }
  const uint32_t id = state->next_file_id++;
  auto inserted = state->file_name_to_id.emplace(file_name, id);
  return inserted.first->second;
}

void EnsureXpBufferModeInitialized(XPControllerState* state,
                                  uint64_t capacity_lines) {
  if (state == nullptr) {
    return;
  }
  if (state->xp_use_small_lru || state->xp_buffer_reserved ||
      state->xp_small_capacity_lines != 0) {
    return;
  }
  if (capacity_lines <= kXpSmallMaxLines) {
    state->xp_use_small_lru = true;
    state->xp_small_capacity_lines = capacity_lines;
    state->xp_small_lru.reserve(static_cast<size_t>(capacity_lines));
  } else {
    state->xp_use_small_lru = false;
    state->xp_buffer_index.reserve(static_cast<size_t>(capacity_lines) * 2);
    state->xp_buffer_reserved = true;
  }
}

inline void TouchXpBufferLineSmall(XPControllerState* state,
                                  const XPBufferLineKey& key) {
  if (state == nullptr) {
    return;
  }
  const size_t cap = static_cast<size_t>(state->xp_small_capacity_lines);
  if (cap == 0) {
    return;
  }
  auto& lru = state->xp_small_lru;

  const size_t n = lru.size();
  for (size_t i = 0; i < n; ++i) {
    if (lru[i] == key) {
      if (i > 0) {
        const XPBufferLineKey tmp = lru[i];
        std::memmove(&lru[1], &lru[0], i * sizeof(XPBufferLineKey));
        lru[0] = tmp;
      }
      return;
    }
  }

  if (n < cap) {
    lru.push_back(key);
    if (n > 0) {
      std::memmove(&lru[1], &lru[0], n * sizeof(XPBufferLineKey));
    }
    lru[0] = key;
    return;
  }

  if (cap > 1) {
    std::memmove(&lru[1], &lru[0], (cap - 1) * sizeof(XPBufferLineKey));
  }
  lru[0] = key;
}

inline void TouchXpBufferLineLarge(XPControllerState* state,
                                  const XPBufferLineKey& key,
                                  uint64_t capacity_lines) {
  if (state == nullptr || capacity_lines == 0) {
    return;
  }
  auto it = state->xp_buffer_index.find(key);
  if (it != state->xp_buffer_index.end()) {
    state->xp_buffer_lru.splice(state->xp_buffer_lru.begin(),
                                state->xp_buffer_lru, it->second);
    it->second = state->xp_buffer_lru.begin();
    return;
  }

  state->xp_buffer_lru.push_front(key);
  state->xp_buffer_index.emplace(state->xp_buffer_lru.front(),
                                 state->xp_buffer_lru.begin());
  while (state->xp_buffer_lru.size() > capacity_lines) {
    const auto& victim = state->xp_buffer_lru.back();
    state->xp_buffer_index.erase(victim);
    state->xp_buffer_lru.pop_back();
  }
}

void TouchXpBufferRangeById(XPControllerState* state, uint32_t file_id,
                            uint64_t offset, uint64_t logical_bytes,
                            uint64_t line_bytes, uint64_t capacity_lines) {
  if (state == nullptr || logical_bytes == 0 || capacity_lines == 0) {
    return;
  }
  EnsureXpBufferModeInitialized(state, capacity_lines);

  const uint64_t first_line = offset / line_bytes;
  const uint64_t line_count = GetLineCount(offset, logical_bytes, line_bytes);

  if (state->xp_use_small_lru) {
    const size_t cap = static_cast<size_t>(state->xp_small_capacity_lines);
    if (cap == 0) {
      return;
    }
    // If we touch >= capacity unique lines, the final LRU must contain exactly
    // the last `cap` lines of the touched range (MRU first). Build it directly.
    if (line_count >= capacity_lines) {
      state->xp_small_lru.clear();
      state->xp_small_lru.resize(cap);
      const uint64_t start =
          first_line + (line_count - capacity_lines);
      for (size_t i = 0; i < cap; ++i) {
        state->xp_small_lru[i] = XPBufferLineKey{
            file_id, start + (capacity_lines - 1 - static_cast<uint64_t>(i))};
      }
      return;
    }
    for (uint64_t i = 0; i < line_count; ++i) {
      TouchXpBufferLineSmall(state, XPBufferLineKey{file_id, first_line + i});
    }
    return;
  }

  if (!state->xp_buffer_reserved) {
    state->xp_buffer_index.reserve(static_cast<size_t>(capacity_lines) * 2);
    state->xp_buffer_reserved = true;
  }
  for (uint64_t i = 0; i < line_count; ++i) {
    TouchXpBufferLineLarge(state, XPBufferLineKey{file_id, first_line + i},
                           capacity_lines);
  }
}

[[maybe_unused]] void TouchXpBufferRange(XPControllerState* state,
                        const std::string& file_name,
                        uint64_t offset, uint64_t logical_bytes,
                        uint64_t line_bytes, uint64_t capacity_lines) {
  if (state == nullptr || logical_bytes == 0 || capacity_lines == 0) {
    return;
  }
  const uint32_t file_id = GetXpFileId(state, file_name);
  TouchXpBufferRangeById(state, file_id, offset, logical_bytes, line_bytes,
                         capacity_lines);
}

uint64_t CountXpBufferHitsById(XPControllerState* state, uint32_t file_id,
                               uint64_t offset, uint64_t logical_bytes,
                               uint64_t line_bytes, uint64_t capacity_lines) {
  if (state == nullptr || logical_bytes == 0) {
    return 0;
  }
  EnsureXpBufferModeInitialized(state, capacity_lines);

  const uint64_t first_line = offset / line_bytes;
  const uint64_t line_count = GetLineCount(offset, logical_bytes, line_bytes);

  if (state->xp_use_small_lru) {
    const uint64_t end = first_line + line_count;
    uint64_t hits = 0;
    for (const auto& e : state->xp_small_lru) {
      if (e.file_id == file_id && e.line_idx >= first_line && e.line_idx < end) {
        ++hits;
      }
    }
    return hits;
  }

  uint64_t hits = 0;
  for (uint64_t i = 0; i < line_count; ++i) {
    if (state->xp_buffer_index.find(XPBufferLineKey{file_id, first_line + i}) !=
        state->xp_buffer_index.end()) {
      ++hits;
    }
  }
  return hits;
}

[[maybe_unused]] uint64_t CountXpBufferHits(XPControllerState* state,
                           const std::string& file_name, uint64_t offset,
                           uint64_t logical_bytes, uint64_t line_bytes,
                           uint64_t capacity_lines) {
  if (state == nullptr || logical_bytes == 0) {
    return 0;
  }
  const uint32_t file_id = GetXpFileId(state, file_name);
  return CountXpBufferHitsById(state, file_id, offset, logical_bytes, line_bytes,
                               capacity_lines);
}

void SimulateXpWriteQueueSubmission(
    const std::string& file_name, uint64_t offset, uint64_t logical_bytes,
    const SimulatedStorageModelOptions& options,
    const std::shared_ptr<SimulatedStorageModelStats>& stats) {
  if (logical_bytes == 0 || stats == nullptr) {
    return;
  }

  const uint64_t submit_latency_ns =
      std::max<uint64_t>(1, options.xp_wpq_submit_ns);
  uint64_t waited_for_queue_ns = 0;
  const uint64_t wpq_depth = std::max<uint64_t>(1, options.xp_wpq_depth);
  const uint64_t wpq_parallelism =
      std::max<uint64_t>(1, options.xp_wpq_parallelism);
  const uint64_t max_merge_window =
      std::max<uint64_t>(1, options.xp_buffer_bytes);
  const uint64_t line_bytes = std::max<uint64_t>(1, options.xp_line_bytes);
  const uint64_t xp_buffer_capacity_lines = GetXpBufferCapacityLines(options);
  const uintptr_t instance_id = GetInstanceId(stats);

  while (true) {
    uint64_t queue_wait_ns = 0;
    uint64_t iteration_now_ns = 0;
    bool enqueued = false;
    {
      std::lock_guard<std::mutex> lk(g_xp_mu);
      XPControllerState& state = g_xp_states[instance_id];
      const uint64_t now_ns =
          GetSimNowNs(instance_id, options.xp_bypass_base_io,
                      options.xp_forced_tag_init_stagger_ns);
      iteration_now_ns = now_ns;
      const uint32_t file_id = GetXpFileId(&state, file_name);
      EnsureServerAvailability(&state.wpq_server_available_ns, wpq_parallelism,
                               now_ns);
      PurgeFinishedWPQ(&state, now_ns);

      if (state.wpq.size() >= wpq_depth) {
        const uint64_t min_finish_ns = MinFinishTime(state.wpq);
        queue_wait_ns = min_finish_ns > now_ns ? min_finish_ns - now_ns : 0;
      } else {
        const uint64_t req_start = offset;
        const uint64_t req_end = GetSaturatedEnd(offset, logical_bytes);
        if (!state.wpq.empty()) {
          XPWriteQueueEntry& tail = state.wpq.back();
          const uint64_t merged_start = std::min(tail.start, req_start);
          const uint64_t merged_end = std::max(tail.end, req_end);
          const bool same_file = tail.file_name == file_name;
          const bool adjacent_or_overlap = req_start <= tail.end;
          const bool in_merge_window =
              (merged_end - merged_start) <= max_merge_window;
          if (same_file && adjacent_or_overlap && in_merge_window) {
            const uint64_t old_media = tail.counted_media_bytes;
            const uint64_t old_service = tail.counted_service_ns;
            tail.start = merged_start;
            tail.end = merged_end;
            const uint64_t merged_logical = merged_end - merged_start;
            tail.counted_media_bytes = GetMediaBytes(options, merged_logical);
            tail.counted_service_ns =
                GetXpServiceNs(options, merged_logical, /*is_write=*/true);
            const uint64_t delta_media =
                tail.counted_media_bytes - old_media;
            const uint64_t delta_service =
                tail.counted_service_ns - old_service;
            tail.finish_ns += delta_service;
            if (tail.server_idx < state.wpq_server_available_ns.size() &&
                state.wpq_server_available_ns[tail.server_idx] < tail.finish_ns) {
              state.wpq_server_available_ns[tail.server_idx] = tail.finish_ns;
            }
            if (options.xp_share_buffer_between_rw) {
              TouchXpBufferRangeById(&state, file_id, merged_start, merged_logical,
                                     line_bytes, xp_buffer_capacity_lines);
            }
            stats->media_write_bytes.fetch_add(delta_media);
            stats->simulated_write_media_delay_ns.fetch_add(delta_service);
            UpdateAtomicMinNonZero(&stats->simulated_write_virtual_start_ns,
                                   tail.issue_start_ns);
            UpdateAtomicMax(&stats->simulated_write_virtual_end_ns,
                            tail.finish_ns);
            enqueued = true;
          }
        }

        if (!enqueued) {
          const uint64_t media_bytes = GetMediaBytes(options, logical_bytes);
          const uint64_t service_ns =
              GetXpServiceNs(options, logical_bytes, /*is_write=*/true);
          uint32_t server_idx = 0;
          uint64_t server_available_ns = 0;
          auto sid_it = state.write_stream_server_by_file_id.find(file_id);
          if (sid_it != state.write_stream_server_by_file_id.end() &&
              !state.wpq_server_available_ns.empty()) {
            server_idx =
                sid_it->second % static_cast<uint32_t>(state.wpq_server_available_ns.size());
            server_available_ns = state.wpq_server_available_ns[server_idx];
          } else {
            const auto chosen = SelectEarliestServerWithHint(
                state.wpq_server_available_ns, state.wpq_rr_cursor);
            server_idx = chosen.first;
            server_available_ns = chosen.second;
            if (!state.wpq_server_available_ns.empty()) {
              state.wpq_rr_cursor =
                  (server_idx + 1) %
                  static_cast<uint32_t>(state.wpq_server_available_ns.size());
            }
            state.write_stream_server_by_file_id[file_id] = server_idx;
          }
          const uint64_t start_ns = std::max(now_ns, server_available_ns);
          const uint64_t finish_ns = start_ns + service_ns;
          state.wpq_server_available_ns[server_idx] = finish_ns;
          if (options.xp_share_buffer_between_rw) {
            TouchXpBufferRangeById(&state, file_id, req_start, logical_bytes,
                                   line_bytes, xp_buffer_capacity_lines);
          }
          state.wpq.push_back(XPWriteQueueEntry{
              file_name, req_start, req_end, finish_ns, media_bytes, service_ns,
              start_ns, server_idx});
          stats->media_write_bytes.fetch_add(media_bytes);
          stats->simulated_write_media_delay_ns.fetch_add(service_ns);
          stats->xp_buffer_flushes.fetch_add(1);
          UpdateAtomicMinNonZero(&stats->simulated_write_virtual_start_ns,
                                 start_ns);
          UpdateAtomicMax(&stats->simulated_write_virtual_end_ns, finish_ns);
          enqueued = true;
        }

        if (enqueued) {
          stats->logical_write_bytes.fetch_add(logical_bytes);
          stats->write_ops.fetch_add(1);
          stats->simulated_write_queue_delay_ns.fetch_add(waited_for_queue_ns);
          stats->simulated_write_delay_ns.fetch_add(submit_latency_ns);
        }
      }
    }

    if (enqueued) {
      AdvanceSimNowNs(instance_id, options.xp_bypass_base_io,
                      iteration_now_ns + waited_for_queue_ns +
                          submit_latency_ns);
      if (options.xp_bypass_base_io) {
        AccountInjectedDelayOnly(submit_latency_ns);
      } else {
        WaitForNanoseconds(submit_latency_ns, options.xp_busy_wait);
      }
      return;
    }
    if (queue_wait_ns > 0) {
      AdvanceSimNowNs(instance_id, options.xp_bypass_base_io,
                      iteration_now_ns + queue_wait_ns);
      if (options.xp_bypass_base_io) {
        AccountInjectedDelayOnly(queue_wait_ns);
      } else {
        WaitForNanoseconds(queue_wait_ns, options.xp_busy_wait);
      }
      waited_for_queue_ns += queue_wait_ns;
    }
  }
}

void SimulateXpReadQueueServe(
    uint32_t file_id, uint64_t offset, uint64_t logical_bytes,
    const SimulatedStorageModelOptions& options,
    const std::shared_ptr<SimulatedStorageModelStats>& stats) {
  if (logical_bytes == 0 || stats == nullptr) {
    return;
  }
  if (file_id == 0) {
    // File id should be assigned at file-open time. Keep a safe non-zero id to
    // avoid pathological keying behavior if callers forget to pass it.
    file_id = 1;
  }

  const uint64_t rpq_depth = std::max<uint64_t>(1, options.xp_rpq_depth);
  const uint64_t rpq_parallelism =
      std::max<uint64_t>(1, options.xp_rpq_parallelism);
  const uint64_t line_bytes = std::max<uint64_t>(1, options.xp_line_bytes);
  const uint64_t xp_buffer_capacity_lines = GetXpBufferCapacityLines(options);
  const uintptr_t instance_id = GetInstanceId(stats);
  const uint64_t stream_tag = GetThreadStreamTag();
  while (true) {
    uint64_t wait_for_depth_ns = 0;
    uint64_t iteration_now_ns = 0;
    uint64_t total_delay_ns = 0;
    uint64_t queue_delay_ns = 0;
    uint64_t service_delay_ns = 0;
    uint64_t xp_service_delay_ns = 0;
    uint64_t device_fixed_overhead_ns = 0;
    uint64_t dram_delay_ns = 0;
    uint64_t arb_delay_ns = 0;
    uint64_t media_bytes = 0;
    bool scheduled = false;
    bool prefetch_hit = false;
    uint64_t hit_lines = 0;
    {
      std::lock_guard<std::mutex> lk(g_xp_mu);
      XPControllerState& state = g_xp_states[instance_id];
      const uint64_t now_ns =
          GetSimNowNs(instance_id, options.xp_bypass_base_io,
                      options.xp_forced_tag_init_stagger_ns);
      iteration_now_ns = now_ns;
      EnsureServerAvailability(&state.rpq_server_available_ns, rpq_parallelism,
                               now_ns);
      PurgeFinishedRPQ(&state, now_ns);

      if (state.rpq_finish_times.size() >= rpq_depth) {
        const uint64_t min_finish_ns = MinFinishTime(state.rpq_finish_times);
        wait_for_depth_ns = min_finish_ns > now_ns ? min_finish_ns - now_ns : 0;
      } else {
        const XPStreamId stream_id = GetOrAssignXpStreamId(&state, stream_tag);
        if (state.read_stream_states_by_id.size() <= static_cast<size_t>(stream_id)) {
          state.read_stream_states_by_id.resize(static_cast<size_t>(stream_id) + 1);
        }
        XPReadStreamState& stream = state.read_stream_states_by_id[stream_id];
        const uint64_t end = GetSaturatedEnd(offset, logical_bytes);
        const bool same_file = stream.has_last && stream.last_file_id == file_id;
        const bool sequential = same_file && offset == stream.last_end;
        const bool first_read = !stream.has_last;
        const bool is_random = stream.has_last && !sequential;
        const bool seq_for_dram = first_read || sequential;
        dram_delay_ns = GetDramReadBaseNs(options, seq_for_dram);
        const uint64_t total_lines =
            GetLineCount(offset, logical_bytes, line_bytes);
        if (options.xp_enable_prefetch) {
          hit_lines =
              CountXpBufferHitsById(&state, file_id, offset, logical_bytes,
                                    line_bytes, xp_buffer_capacity_lines);
        }
        prefetch_hit = hit_lines > 0;
        const uint64_t miss_lines =
            total_lines > hit_lines ? total_lines - hit_lines : 0;
        media_bytes = miss_lines * line_bytes;
        if (miss_lines == 0) {
          xp_service_delay_ns =
              std::max<uint64_t>(1, options.xp_prefetch_hit_ns);
        } else {
          if (options.xp_use_dimm_device_model) {
            device_fixed_overhead_ns = options.dimm_fixed_read_overhead_ns;
            // Model a request-level device round-trip to fetch XPLine(s).
            // Real devices can pipeline multiple XPLine reads for one 4KB
            // request, so the latency should not necessarily scale as
            // miss_lines * xp_latency_ns.
            const uint64_t command_ns = std::max<uint64_t>(1, options.xp_latency_ns);
            const double bw_gbps =
                GetDimmEffectiveBandwidthGbps(options, /*is_write=*/false, is_random);
            const uint64_t transfer_ns = GetDimmTransferTimeNs(media_bytes, bw_gbps);
            if (device_fixed_overhead_ns > 0) {
              stats->simulated_read_fixed_delay_ns.fetch_add(device_fixed_overhead_ns);
            }
            xp_service_delay_ns = command_ns + transfer_ns;
          } else {
            xp_service_delay_ns =
                GetXpServiceNs(options, media_bytes, /*is_write=*/false);
          }
          if (hit_lines > 0) {
            xp_service_delay_ns +=
                std::max<uint64_t>(1, options.xp_prefetch_hit_ns);
          }
        }
        const uint64_t outstanding_reads = state.rpq_finish_times.size();
        arb_delay_ns = outstanding_reads * options.xp_rpq_arb_ns;
        service_delay_ns = device_fixed_overhead_ns + xp_service_delay_ns +
                           dram_delay_ns + arb_delay_ns;
        const auto [server_idx, server_available_ns] =
            SelectEarliestServerWithHint(state.rpq_server_available_ns,
                                         state.rpq_rr_cursor);
        if (!state.rpq_server_available_ns.empty()) {
          state.rpq_rr_cursor =
              (server_idx + 1) %
              static_cast<uint32_t>(state.rpq_server_available_ns.size());
        }
        const uint64_t start_ns = std::max(now_ns, server_available_ns);
        const uint64_t finish_ns = start_ns + service_delay_ns;
        queue_delay_ns = start_ns - now_ns;
        total_delay_ns = finish_ns - now_ns;
        state.rpq_server_available_ns[server_idx] = finish_ns;
        state.rpq_finish_times.push_back(finish_ns);

        if (options.xp_enable_prefetch) {
          // Read miss refill / cache residency.
          TouchXpBufferRangeById(&state, file_id, offset, logical_bytes,
                                 line_bytes, xp_buffer_capacity_lines);
        }
        stream.has_last = true;
        stream.last_file_id = file_id;
        stream.last_end = end;
        if (options.xp_enable_prefetch) {
          // Sequential prefetch for the next XPBuffer window.
          if (first_read || sequential) {
            TouchXpBufferRangeById(&state, file_id, end, options.xp_buffer_bytes,
                                   line_bytes, xp_buffer_capacity_lines);
          }
        }

        stats->logical_read_bytes.fetch_add(logical_bytes);
        stats->media_read_bytes.fetch_add(media_bytes);
        stats->read_ops.fetch_add(1);
        stats->simulated_read_queue_delay_ns.fetch_add(queue_delay_ns);
        stats->simulated_read_media_delay_ns.fetch_add(xp_service_delay_ns);
        stats->simulated_read_dram_delay_ns.fetch_add(dram_delay_ns);
        stats->simulated_read_arb_delay_ns.fetch_add(arb_delay_ns);
        stats->simulated_read_delay_ns.fetch_add(total_delay_ns);
        UpdateAtomicMinNonZero(&stats->simulated_read_virtual_start_ns,
                               start_ns);
        UpdateAtomicMax(&stats->simulated_read_virtual_end_ns, finish_ns);
        if (prefetch_hit) {
          stats->read_prefetch_hits.fetch_add(1);
        }
        scheduled = true;
      }
    }

    if (scheduled) {
      AdvanceSimNowNs(instance_id, options.xp_bypass_base_io,
                      iteration_now_ns + total_delay_ns);
      if (options.xp_bypass_base_io) {
        AccountInjectedDelayOnly(total_delay_ns);
      } else {
        WaitForNanoseconds(total_delay_ns, options.xp_busy_wait);
      }
      return;
    }
    if (wait_for_depth_ns > 0) {
      AdvanceSimNowNs(instance_id, options.xp_bypass_base_io,
                      iteration_now_ns + wait_for_depth_ns);
      if (options.xp_bypass_base_io) {
        AccountInjectedDelayOnly(wait_for_depth_ns);
      } else {
        WaitForNanoseconds(wait_for_depth_ns, options.xp_busy_wait);
      }
    }
  }
}

void SimulateDimmReadQueueServe(
    uint32_t file_id, uint64_t offset, uint64_t logical_bytes,
    const SimulatedStorageModelOptions& options,
    const std::shared_ptr<SimulatedStorageModelStats>& stats) {
  if (logical_bytes == 0 || stats == nullptr) {
    return;
  }
  if (file_id == 0) {
    file_id = 1;
  }

  const uint64_t fixed_overhead_ns = options.dimm_fixed_read_overhead_ns;
  const uintptr_t instance_id = GetInstanceId(stats);
  if (fixed_overhead_ns > 0) {
    const uint64_t begin_ns =
        GetSimNowNs(instance_id, options.xp_bypass_base_io,
                    options.xp_forced_tag_init_stagger_ns);
    AdvanceSimNowNs(instance_id, options.xp_bypass_base_io,
                    begin_ns + fixed_overhead_ns);
    if (options.xp_bypass_base_io) {
      AccountInjectedDelayOnly(fixed_overhead_ns);
    } else {
      WaitForNanoseconds(fixed_overhead_ns, options.xp_busy_wait);
    }
  }
  stats->simulated_read_fixed_delay_ns.fetch_add(fixed_overhead_ns);

  const uint64_t rpq_depth = std::max<uint64_t>(1, options.xp_rpq_depth);
  const uint64_t stream_tag = GetThreadStreamTag();
  while (true) {
    uint64_t wait_for_depth_ns = 0;
    uint64_t iteration_now_ns = 0;
    uint64_t queue_delay_ns = 0;
    uint64_t transfer_delay_ns = 0;
    uint64_t media_bytes = 0;
    bool scheduled = false;
    uint64_t start_ns = 0;
    uint64_t finish_ns = 0;
    {
      std::lock_guard<std::mutex> lk(g_dimm_mu);
      DimmControllerState& state = g_dimm_states[instance_id];
      const uint64_t now_ns =
          GetSimNowNs(instance_id, options.xp_bypass_base_io,
                      options.xp_forced_tag_init_stagger_ns);
      iteration_now_ns = now_ns;
      EnsureServerAvailability(&state.read_server_available_ns,
                               /*parallelism=*/1, now_ns);
      PurgeFinishedTimes(&state.read_finish_times, now_ns);

      if (state.read_finish_times.size() >= rpq_depth) {
        const uint64_t min_finish_ns = MinFinishTime(state.read_finish_times);
        wait_for_depth_ns = min_finish_ns > now_ns ? min_finish_ns - now_ns : 0;
      } else {
        const uint32_t stream_id = GetOrAssignDimmStreamId(&state, stream_tag);
        if (state.read_stream_states_by_id.size() <= static_cast<size_t>(stream_id)) {
          state.read_stream_states_by_id.resize(static_cast<size_t>(stream_id) + 1);
        }
        DimmStreamState& stream = state.read_stream_states_by_id[stream_id];
        const uint64_t end = GetSaturatedEnd(offset, logical_bytes);
        const bool same_file = stream.has_last && stream.last_file_id == file_id;
        const bool sequential = same_file && offset == stream.last_end;
        const bool is_random = stream.has_last && !sequential;
        stream.has_last = true;
        stream.last_file_id = file_id;
        stream.last_end = end;

        media_bytes = GetDimmEffectiveMediaBytes(options, logical_bytes, is_random);
        const double bw_gbps =
            GetDimmEffectiveBandwidthGbps(options, /*is_write=*/false, is_random);
        transfer_delay_ns = GetDimmTransferTimeNs(media_bytes, bw_gbps);

        const auto [server_idx, server_available_ns] =
            SelectEarliestServerWithHint(state.read_server_available_ns,
                                         state.read_rr_cursor);
        if (!state.read_server_available_ns.empty()) {
          state.read_rr_cursor =
              (server_idx + 1) %
              static_cast<uint32_t>(state.read_server_available_ns.size());
        }
        start_ns = std::max(now_ns, server_available_ns);
        finish_ns = start_ns + transfer_delay_ns;
        queue_delay_ns = start_ns - now_ns;
        state.read_server_available_ns[server_idx] = finish_ns;
        state.read_finish_times.push_back(finish_ns);

        stats->logical_read_bytes.fetch_add(logical_bytes);
        stats->media_read_bytes.fetch_add(media_bytes);
        stats->read_ops.fetch_add(1);
        stats->simulated_read_queue_delay_ns.fetch_add(queue_delay_ns);
        stats->simulated_read_media_delay_ns.fetch_add(transfer_delay_ns);
        stats->simulated_read_delay_ns.fetch_add(fixed_overhead_ns +
                                                 queue_delay_ns +
                                                 transfer_delay_ns);
        UpdateAtomicMinNonZero(&stats->simulated_read_virtual_start_ns, start_ns);
        UpdateAtomicMax(&stats->simulated_read_virtual_end_ns, finish_ns);
        scheduled = true;
      }
    }

    if (scheduled) {
      const uint64_t device_delay_ns = queue_delay_ns + transfer_delay_ns;
      AdvanceSimNowNs(instance_id, options.xp_bypass_base_io,
                      iteration_now_ns + device_delay_ns);
      if (options.xp_bypass_base_io) {
        AccountInjectedDelayOnly(device_delay_ns);
      } else {
        WaitForNanoseconds(device_delay_ns, options.xp_busy_wait);
      }
      return;
    }
    if (wait_for_depth_ns > 0) {
      AdvanceSimNowNs(instance_id, options.xp_bypass_base_io,
                      iteration_now_ns + wait_for_depth_ns);
      if (options.xp_bypass_base_io) {
        AccountInjectedDelayOnly(wait_for_depth_ns);
      } else {
        WaitForNanoseconds(wait_for_depth_ns, options.xp_busy_wait);
      }
    }
  }
}

void SimulateDimmWriteQueueServe(
    const std::string& file_name, uint64_t offset, uint64_t logical_bytes,
    const SimulatedStorageModelOptions& options,
    const std::shared_ptr<SimulatedStorageModelStats>& stats) {
  if (logical_bytes == 0 || stats == nullptr) {
    return;
  }

  const uint64_t fixed_overhead_ns = options.dimm_fixed_write_overhead_ns;
  const uintptr_t instance_id = GetInstanceId(stats);
  if (fixed_overhead_ns > 0) {
    const uint64_t begin_ns =
        GetSimNowNs(instance_id, options.xp_bypass_base_io,
                    options.xp_forced_tag_init_stagger_ns);
    AdvanceSimNowNs(instance_id, options.xp_bypass_base_io,
                    begin_ns + fixed_overhead_ns);
    if (options.xp_bypass_base_io) {
      AccountInjectedDelayOnly(fixed_overhead_ns);
    } else {
      WaitForNanoseconds(fixed_overhead_ns, options.xp_busy_wait);
    }
  }
  stats->simulated_write_fixed_delay_ns.fetch_add(fixed_overhead_ns);

  const uint64_t wpq_depth = std::max<uint64_t>(1, options.xp_wpq_depth);
  const uint64_t stream_tag = GetThreadStreamTag();
  while (true) {
    uint64_t wait_for_depth_ns = 0;
    uint64_t iteration_now_ns = 0;
    uint64_t queue_delay_ns = 0;
    uint64_t transfer_delay_ns = 0;
    uint64_t media_bytes = 0;
    bool scheduled = false;
    uint64_t start_ns = 0;
    uint64_t finish_ns = 0;
    {
      std::lock_guard<std::mutex> lk(g_dimm_mu);
      DimmControllerState& state = g_dimm_states[instance_id];
      const uint64_t now_ns =
          GetSimNowNs(instance_id, options.xp_bypass_base_io,
                      options.xp_forced_tag_init_stagger_ns);
      iteration_now_ns = now_ns;
      EnsureServerAvailability(&state.write_server_available_ns,
                               /*parallelism=*/1, now_ns);
      PurgeFinishedTimes(&state.write_finish_times, now_ns);

      if (state.write_finish_times.size() >= wpq_depth) {
        const uint64_t min_finish_ns = MinFinishTime(state.write_finish_times);
        wait_for_depth_ns = min_finish_ns > now_ns ? min_finish_ns - now_ns : 0;
      } else {
        const uint32_t file_id = GetDimmFileId(&state, file_name);
        const uint32_t stream_id = GetOrAssignDimmStreamId(&state, stream_tag);
        if (state.write_stream_states_by_id.size() <= static_cast<size_t>(stream_id)) {
          state.write_stream_states_by_id.resize(static_cast<size_t>(stream_id) + 1);
        }
        DimmStreamState& stream = state.write_stream_states_by_id[stream_id];
        const uint64_t end = GetSaturatedEnd(offset, logical_bytes);
        const bool same_file = stream.has_last && stream.last_file_id == file_id;
        const bool sequential = same_file && offset == stream.last_end;
        const bool is_random = stream.has_last && !sequential;
        stream.has_last = true;
        stream.last_file_id = file_id;
        stream.last_end = end;

        media_bytes = GetDimmEffectiveMediaBytes(options, logical_bytes, is_random);
        const double bw_gbps =
            GetDimmEffectiveBandwidthGbps(options, /*is_write=*/true, is_random);
        transfer_delay_ns = GetDimmTransferTimeNs(media_bytes, bw_gbps);

        const auto [server_idx, server_available_ns] =
            SelectEarliestServerWithHint(state.write_server_available_ns,
                                         state.write_rr_cursor);
        if (!state.write_server_available_ns.empty()) {
          state.write_rr_cursor =
              (server_idx + 1) %
              static_cast<uint32_t>(state.write_server_available_ns.size());
        }
        start_ns = std::max(now_ns, server_available_ns);
        finish_ns = start_ns + transfer_delay_ns;
        queue_delay_ns = start_ns - now_ns;
        state.write_server_available_ns[server_idx] = finish_ns;
        state.write_finish_times.push_back(finish_ns);

        stats->logical_write_bytes.fetch_add(logical_bytes);
        stats->media_write_bytes.fetch_add(media_bytes);
        stats->write_ops.fetch_add(1);
        stats->simulated_write_queue_delay_ns.fetch_add(queue_delay_ns);
        stats->simulated_write_media_delay_ns.fetch_add(transfer_delay_ns);
        stats->simulated_write_delay_ns.fetch_add(fixed_overhead_ns +
                                                  queue_delay_ns +
                                                  transfer_delay_ns);
        UpdateAtomicMinNonZero(&stats->simulated_write_virtual_start_ns, start_ns);
        UpdateAtomicMax(&stats->simulated_write_virtual_end_ns, finish_ns);
        scheduled = true;
      }
    }

    if (scheduled) {
      const uint64_t device_delay_ns = queue_delay_ns + transfer_delay_ns;
      AdvanceSimNowNs(instance_id, options.xp_bypass_base_io,
                      iteration_now_ns + device_delay_ns);
      if (options.xp_bypass_base_io) {
        AccountInjectedDelayOnly(device_delay_ns);
      } else {
        WaitForNanoseconds(device_delay_ns, options.xp_busy_wait);
      }
      return;
    }
    if (wait_for_depth_ns > 0) {
      AdvanceSimNowNs(instance_id, options.xp_bypass_base_io,
                      iteration_now_ns + wait_for_depth_ns);
      if (options.xp_bypass_base_io) {
        AccountInjectedDelayOnly(wait_for_depth_ns);
      } else {
        WaitForNanoseconds(wait_for_depth_ns, options.xp_busy_wait);
      }
    }
  }
}
}  // namespace

void SetSimulatedFsThreadTagForCurrentThread(uint64_t tag) {
  g_xp_forced_stream_tag = tag;
  g_xp_forced_stream_tag_valid = true;
}

void ClearSimulatedFsThreadTagForCurrentThread() {
  g_xp_forced_stream_tag_valid = false;
}

void RegisterSimulatedFsFileLevel(const std::string& file_name, int level) {
  std::lock_guard<std::mutex> lk(g_xp_file_levels_mu);
  g_xp_file_levels[file_name] = level;
}

void UnregisterSimulatedFsFileLevel(const std::string& file_name) {
  std::lock_guard<std::mutex> lk(g_xp_file_levels_mu);
  g_xp_file_levels.erase(file_name);
}

void ClearSimulatedFsFileLevels() {
  std::lock_guard<std::mutex> lk(g_xp_file_levels_mu);
  g_xp_file_levels.clear();
}

// The metadata file format: each line is a full filename of a file which is
// warm
SimulatedHybridFileSystem::SimulatedHybridFileSystem(
    const std::shared_ptr<FileSystem>& base,
    const std::string& metadata_file_name, int throughput_multiplier,
    bool is_full_fs_warm, SimulatedStorageModelOptions model_options)
    : FileSystemWrapper(base),
      // Limit to 100 requests per second.
      rate_limiter_(NewGenericRateLimiter(
          int64_t{throughput_multiplier} * kDummyBytesPerUs *
              kUsPerSec /* rate_bytes_per_sec */,
          1000 /* refill_period_us */)),
      metadata_file_name_(metadata_file_name),
      name_("SimulatedHybridFileSystem: " + std::string(target()->Name())),
      is_full_fs_warm_(is_full_fs_warm),
      model_options_(std::move(model_options)),
      stats_(std::make_shared<SimulatedStorageModelStats>()) {
  if (model_options_.use_xp_model && model_options_.use_dimm_model) {
    std::cerr << "[simfs] invalid config: use_xp_model and use_dimm_model "
                 "are mutually exclusive\n";
    std::exit(1);
  }
  model_options_.xp_line_bytes = std::max<uint64_t>(1, model_options_.xp_line_bytes);
  model_options_.xp_service_bytes =
      std::max<uint64_t>(1, model_options_.xp_service_bytes);
  model_options_.xp_buffer_bytes =
      std::max<uint64_t>(1, model_options_.xp_buffer_bytes);
  model_options_.xp_latency_ns = std::max<uint64_t>(1, model_options_.xp_latency_ns);
  model_options_.xp_rpq_depth = std::max<uint64_t>(1, model_options_.xp_rpq_depth);
  model_options_.xp_wpq_depth = std::max<uint64_t>(1, model_options_.xp_wpq_depth);
  model_options_.xp_rpq_parallelism =
      std::max<uint64_t>(1, model_options_.xp_rpq_parallelism);
  model_options_.xp_wpq_parallelism =
      std::max<uint64_t>(1, model_options_.xp_wpq_parallelism);
  model_options_.xp_read_line_parallelism =
      std::max<uint64_t>(1, model_options_.xp_read_line_parallelism);
  model_options_.xp_write_line_parallelism =
      std::max<uint64_t>(1, model_options_.xp_write_line_parallelism);
  model_options_.xp_tmpfs_root =
      NormalizeTmpfsRoot(std::move(model_options_.xp_tmpfs_root));
  model_options_.dimm_seq_read_bw_gbps =
      std::max(1e-9, model_options_.dimm_seq_read_bw_gbps);
  model_options_.dimm_seq_write_bw_gbps =
      std::max(1e-9, model_options_.dimm_seq_write_bw_gbps);
  model_options_.dimm_rand_bw_scale =
      ClampDouble(model_options_.dimm_rand_bw_scale, 0.0, 1.0);
  model_options_.dimm_sub_line_random_media_amp =
      std::max(1.0, model_options_.dimm_sub_line_random_media_amp);

  if (IsTmpfsRedirectEnabled()) {
    const IOStatus s = EnsureDirRecursive(model_options_.xp_tmpfs_root, nullptr);
    if (!s.ok()) {
      std::cerr << "[simfs] failed to initialize tmpfs root '"
                << model_options_.xp_tmpfs_root
                << "': " << s.ToString() << "\n";
      std::exit(1);
    }
  }

  if (model_options_.monitor_enable &&
      (!model_options_.monitor_window_csv.empty() ||
       !model_options_.monitor_stage_csv.empty())) {
    if (!model_options_.monitor_window_csv.empty()) {
      const IOStatus s =
          EnsureParentDir(model_options_.monitor_window_csv, nullptr);
      if (!s.ok()) {
        std::cerr << "[simfs] failed to create monitor window csv parent dir '"
                  << model_options_.monitor_window_csv
                  << "': " << s.ToString() << "\n";
        std::exit(1);
      }
    }
    if (!model_options_.monitor_stage_csv.empty()) {
      const IOStatus s =
          EnsureParentDir(model_options_.monitor_stage_csv, nullptr);
      if (!s.ok()) {
        std::cerr << "[simfs] failed to create monitor stage csv parent dir '"
                  << model_options_.monitor_stage_csv
                  << "': " << s.ToString() << "\n";
        std::exit(1);
      }
    }
    latency_monitor_ =
        std::make_shared<SimulatedFsLatencyMonitor>(model_options_);
  }

  if (metadata_file_name.empty()) {
    return;
  }

  IOStatus s = base->FileExists(metadata_file_name, IOOptions(), nullptr);
  if (s.IsNotFound()) {
    return;
  }
  std::string metadata;
  s = ReadFileToString(base.get(), metadata_file_name, &metadata);
  if (!s.ok()) {
    fprintf(stderr, "Error reading from file %s: %s",
            metadata_file_name.c_str(), s.ToString().c_str());
    // Exit rather than assert as this file system is built to run with
    // benchmarks, which usually run on release mode.
    std::exit(1);
  }
  std::istringstream input;
  input.str(metadata);
  std::string line;
  while (std::getline(input, line)) {
    fprintf(stderr, "Warm file %s\n", line.c_str());
    warm_file_set_.insert(line);
  }
}

// Need to write out the metadata file to file. See comment of
// SimulatedHybridFileSystem::SimulatedHybridFileSystem() for format of the
// file.
SimulatedHybridFileSystem::~SimulatedHybridFileSystem() {
  if (latency_monitor_ != nullptr) {
    latency_monitor_->Stop();
  }
  MaybeWriteModelStats();
  CleanupXpControllerState(stats_);
  CleanupDimmControllerState(stats_);

  if (metadata_file_name_.empty()) {
    return;
  }
  std::string metadata;
  for (const auto& f : warm_file_set_) {
    metadata += f;
    metadata += "\n";
  }
  IOOptions opts;
  IOStatus s =
      WriteStringToFile(target(), metadata, metadata_file_name_, true, opts);
  if (!s.ok()) {
    fprintf(stderr, "Error writing to file %s: %s", metadata_file_name_.c_str(),
            s.ToString().c_str());
  }
}

void SimulatedHybridFileSystem::SetMonitorStartTimeMicros(uint64_t start_us) {
  if (latency_monitor_ != nullptr) {
    latency_monitor_->SetStartTimeMicros(start_us);
  }
}

void SimulatedHybridFileSystem::MaybeWriteModelStats() const {
  if (model_options_.stats_file.empty() || stats_ == nullptr) {
    return;
  }
  std::ofstream out(model_options_.stats_file,
                    std::ofstream::out | std::ofstream::trunc);
  if (!out.good()) {
    fprintf(stderr, "Error writing model stats file %s\n",
            model_options_.stats_file.c_str());
    return;
  }
  const uint64_t logical_read = stats_->logical_read_bytes.load();
  const uint64_t logical_write = stats_->logical_write_bytes.load();
  const uint64_t media_read = stats_->media_read_bytes.load();
  const uint64_t media_write = stats_->media_write_bytes.load();
  const uint64_t read_ops = stats_->read_ops.load();
  const uint64_t write_ops = stats_->write_ops.load();
  const uint64_t read_delay = stats_->simulated_read_delay_ns.load();
  const uint64_t write_delay = stats_->simulated_write_delay_ns.load();
  const uint64_t read_fixed_delay =
      stats_->simulated_read_fixed_delay_ns.load();
  const uint64_t write_fixed_delay =
      stats_->simulated_write_fixed_delay_ns.load();
  const uint64_t read_queue_delay =
      stats_->simulated_read_queue_delay_ns.load();
  const uint64_t read_media_delay =
      stats_->simulated_read_media_delay_ns.load();
  const uint64_t read_dram_delay =
      stats_->simulated_read_dram_delay_ns.load();
  const uint64_t read_arb_delay = stats_->simulated_read_arb_delay_ns.load();
  const uint64_t write_queue_delay =
      stats_->simulated_write_queue_delay_ns.load();
  const uint64_t write_media_delay =
      stats_->simulated_write_media_delay_ns.load();
  const uint64_t read_prefetch_hits = stats_->read_prefetch_hits.load();
  const uint64_t flushes = stats_->xp_buffer_flushes.load();
  const uint64_t read_virtual_start =
      stats_->simulated_read_virtual_start_ns.load();
  const uint64_t read_virtual_end = stats_->simulated_read_virtual_end_ns.load();
  const uint64_t write_virtual_start =
      stats_->simulated_write_virtual_start_ns.load();
  const uint64_t write_virtual_end =
      stats_->simulated_write_virtual_end_ns.load();
  const uint64_t tmpfs_read_opens = stats_->tmpfs_read_opens.load();
  const uint64_t tmpfs_write_opens = stats_->tmpfs_write_opens.load();

  out << "use_xp_model=" << (model_options_.use_xp_model ? 1 : 0) << "\n";
  out << "use_dimm_model=" << (model_options_.use_dimm_model ? 1 : 0) << "\n";
  out << "xp_line_bytes=" << model_options_.xp_line_bytes << "\n";
  out << "xp_service_bytes=" << model_options_.xp_service_bytes << "\n";
  out << "xp_buffer_bytes=" << model_options_.xp_buffer_bytes << "\n";
  out << "xp_latency_ns=" << model_options_.xp_latency_ns << "\n";
  out << "xp_rpq_depth=" << model_options_.xp_rpq_depth << "\n";
  out << "xp_wpq_depth=" << model_options_.xp_wpq_depth << "\n";
  out << "xp_wpq_submit_ns=" << model_options_.xp_wpq_submit_ns << "\n";
  out << "xp_prefetch_hit_ns=" << model_options_.xp_prefetch_hit_ns << "\n";
  out << "dram_read_seq_ns=" << model_options_.dram_read_seq_ns << "\n";
  out << "dram_read_rand_ns=" << model_options_.dram_read_rand_ns << "\n";
  out << "xp_enable_prefetch=" << (model_options_.xp_enable_prefetch ? 1 : 0)
      << "\n";
  out << "xp_share_buffer_between_rw="
      << (model_options_.xp_share_buffer_between_rw ? 1 : 0) << "\n";
  out << "xp_rpq_parallelism=" << model_options_.xp_rpq_parallelism << "\n";
  out << "xp_wpq_parallelism=" << model_options_.xp_wpq_parallelism << "\n";
  out << "xp_read_line_parallelism=" << model_options_.xp_read_line_parallelism
      << "\n";
  out << "xp_write_line_parallelism=" << model_options_.xp_write_line_parallelism
      << "\n";
  out << "xp_rpq_arb_ns=" << model_options_.xp_rpq_arb_ns << "\n";
  if (model_options_.target_levels.empty()) {
    out << "xp_target_levels=" << "\n";
  } else {
    std::vector<int> sorted_levels(model_options_.target_levels.begin(),
                                   model_options_.target_levels.end());
    std::sort(sorted_levels.begin(), sorted_levels.end());
    out << "xp_target_levels=";
    for (size_t i = 0; i < sorted_levels.size(); ++i) {
      if (i > 0) {
        out << ",";
      }
      out << sorted_levels[i];
    }
    out << "\n";
  }
  out << "xp_forced_tag_init_stagger_ns="
      << model_options_.xp_forced_tag_init_stagger_ns << "\n";
  out << "xp_bypass_base_io=" << (model_options_.xp_bypass_base_io ? 1 : 0)
      << "\n";
  out << "xp_mmap_base_io=" << (model_options_.xp_mmap_base_io ? 1 : 0) << "\n";
  out << "xp_busy_wait=" << (model_options_.xp_busy_wait ? 1 : 0) << "\n";
  out << "xp_use_dimm_device_model="
      << (model_options_.xp_use_dimm_device_model ? 1 : 0) << "\n";
  out << "dimm_fixed_read_overhead_ns=" << model_options_.dimm_fixed_read_overhead_ns
      << "\n";
  out << "dimm_fixed_write_overhead_ns="
      << model_options_.dimm_fixed_write_overhead_ns << "\n";
  out << "dimm_seq_read_bw_gbps=" << model_options_.dimm_seq_read_bw_gbps << "\n";
  out << "dimm_seq_write_bw_gbps=" << model_options_.dimm_seq_write_bw_gbps << "\n";
  out << "dimm_rand_bw_scale=" << model_options_.dimm_rand_bw_scale << "\n";
  out << "dimm_sub_line_random_media_amp="
      << model_options_.dimm_sub_line_random_media_amp << "\n";
  out << "xp_redirect_to_tmpfs="
      << (model_options_.xp_redirect_to_tmpfs ? 1 : 0) << "\n";
  out << "xp_tmpfs_root=" << model_options_.xp_tmpfs_root << "\n";
  out << "tmpfs_read_opens=" << tmpfs_read_opens << "\n";
  out << "tmpfs_write_opens=" << tmpfs_write_opens << "\n";
  out << "tmpfs_read_ops=" << stats_->tmpfs_read_ops.load() << "\n";
  out << "tmpfs_read_bytes=" << stats_->tmpfs_read_bytes.load() << "\n";
  out << "base_read_ops=" << stats_->base_read_ops.load() << "\n";
  out << "base_read_bytes=" << stats_->base_read_bytes.load() << "\n";
  out << "tmpfs_prefetch_ops=" << stats_->tmpfs_prefetch_ops.load() << "\n";
  out << "tmpfs_prefetch_bytes=" << stats_->tmpfs_prefetch_bytes.load() << "\n";
  out << "base_prefetch_ops=" << stats_->base_prefetch_ops.load() << "\n";
  out << "base_prefetch_bytes=" << stats_->base_prefetch_bytes.load() << "\n";
  out << "logical_read_bytes=" << logical_read << "\n";
  out << "logical_write_bytes=" << logical_write << "\n";
  out << "media_read_bytes=" << media_read << "\n";
  out << "media_write_bytes=" << media_write << "\n";
  out << "read_ops=" << read_ops << "\n";
  out << "write_ops=" << write_ops << "\n";
  out << "simulated_read_delay_ns=" << read_delay << "\n";
  out << "simulated_write_delay_ns=" << write_delay << "\n";
  out << "simulated_read_fixed_delay_ns=" << read_fixed_delay << "\n";
  out << "simulated_write_fixed_delay_ns=" << write_fixed_delay << "\n";
  out << "simulated_read_queue_delay_ns=" << read_queue_delay << "\n";
  out << "simulated_read_media_delay_ns=" << read_media_delay << "\n";
  out << "simulated_read_dram_delay_ns=" << read_dram_delay << "\n";
  out << "simulated_read_arb_delay_ns=" << read_arb_delay << "\n";
  out << "simulated_write_queue_delay_ns=" << write_queue_delay << "\n";
  out << "simulated_write_media_delay_ns=" << write_media_delay << "\n";
  out << "simulated_read_virtual_start_ns=" << read_virtual_start << "\n";
  out << "simulated_read_virtual_end_ns=" << read_virtual_end << "\n";
  out << "simulated_write_virtual_start_ns=" << write_virtual_start << "\n";
  out << "simulated_write_virtual_end_ns=" << write_virtual_end << "\n";
  out << "read_prefetch_hits=" << read_prefetch_hits << "\n";
  out << "xp_buffer_flushes=" << flushes << "\n";
  const double ewr = logical_write == 0
                         ? 0.0
                         : static_cast<double>(media_write) /
                               static_cast<double>(logical_write);
  out << "ewr=" << ewr << "\n";
}

bool SimulatedHybridFileSystem::ShouldSimulatePath(
    const std::string& fname) const {
  if (model_options_.path_prefix.empty()) {
    // no-op
  } else {
    if (fname.size() < model_options_.path_prefix.size()) {
      return false;
    }
    if (fname.compare(0, model_options_.path_prefix.size(),
                      model_options_.path_prefix) != 0) {
      return false;
    }
  }
  if (!model_options_.target_levels.empty() &&
      (model_options_.use_xp_model || model_options_.use_dimm_model)) {
    if (!IsSstFileName(fname)) {
      return false;
    }
    int level = -1;
    if (!LookupSimulatedFsFileLevel(fname, &level)) {
      // Best-effort fallback: during DB open there might be SST file opens
      // before `db_bench` has seeded the live-file -> level map (from
      // GetLiveFilesMetaData()). Returning false here would silently disable
      // simulation (and tmpfs redirect selection) for those early opens.
      //
      // When tmpfs redirect is enabled, we can avoid incorrectly simulating
      // non-target levels by checking whether the SST physically exists in
      // tmpfs. This keeps L5/L6 (SSD) reads from being wrapped by the NVM model
      // during DB open, while still allowing L0-L4 (tmpfs) SSTs to be opened.
      if (IsTmpfsRedirectEnabled()) {
        return TmpfsFileExists(fname);
      }
      // Without tmpfs redirect we have no reliable signal, so prefer to
      // simulate in this ambiguous state; once the map is seeded, subsequent
      // opens will be filtered precisely by `target_levels`.
      return true;
    }
    return model_options_.target_levels.find(level) !=
           model_options_.target_levels.end();
  }
  return true;
}

bool SimulatedHybridFileSystem::IsTmpfsRedirectEnabled() const {
  return model_options_.xp_redirect_to_tmpfs &&
         !model_options_.xp_tmpfs_root.empty();
}

std::string SimulatedHybridFileSystem::NormalizeTmpfsRoot(std::string root) const {
  if (root.empty()) {
    root = "/dev/shm/tmpfs";
  }
  while (root.size() > 1 && root.back() == '/') {
    root.pop_back();
  }
  if (root.empty()) {
    root = "/dev/shm/tmpfs";
  }
  return root;
}

std::string SimulatedHybridFileSystem::ToTmpfsPath(const std::string& fname) const {
  if (!IsTmpfsRedirectEnabled() || fname.empty()) {
    return fname;
  }
  if (IsUnderTmpfsRoot(fname)) {
    return fname;
  }
  if (!fname.empty() && fname[0] == '/') {
    return model_options_.xp_tmpfs_root + fname;
  }
  return model_options_.xp_tmpfs_root + "/" + fname;
}

bool SimulatedHybridFileSystem::IsUnderTmpfsRoot(const std::string& fname) const {
  const std::string& root = model_options_.xp_tmpfs_root;
  if (root.empty() || fname.size() < root.size()) {
    return false;
  }
  if (fname.compare(0, root.size(), root) != 0) {
    return false;
  }
  return fname.size() == root.size() || fname[root.size()] == '/';
}

bool SimulatedHybridFileSystem::TmpfsFileExists(const std::string& fname) const {
  if (!IsTmpfsRedirectEnabled()) {
    return false;
  }
  const std::string tmpfs_path = ToTmpfsPath(fname);
  return target()->FileExists(tmpfs_path, IOOptions(), nullptr).ok();
}

bool SimulatedHybridFileSystem::ShouldPersistToTmpfs(
    const std::string& fname) const {
  if (!IsTmpfsRedirectEnabled()) {
    return false;
  }
  if (IsUnderTmpfsRoot(fname)) {
    return true;
  }
  // Redirect only the files that are selected by the current simulation
  // filters (e.g., SST levels). This keeps metadata/WAL/log files on the base
  // filesystem and avoids tmpfs capacity blowups when only a subset of data is
  // meant to live on "NVM".
  return ShouldSimulatePath(fname);
}

std::string SimulatedHybridFileSystem::ResolveReadPath(
    const std::string& fname) const {
  if (!IsTmpfsRedirectEnabled() || IsUnderTmpfsRoot(fname)) {
    return fname;
  }
  if (TmpfsFileExists(fname)) {
    return ToTmpfsPath(fname);
  }
  return fname;
}

std::string SimulatedHybridFileSystem::ResolveWritePath(
    const std::string& fname) const {
  if (!IsTmpfsRedirectEnabled() || IsUnderTmpfsRoot(fname)) {
    return fname;
  }
  if (ShouldPersistToTmpfs(fname)) {
    return ToTmpfsPath(fname);
  }
  return fname;
}

IOStatus SimulatedHybridFileSystem::EnsureDirRecursive(
    const std::string& dir, IODebugContext* dbg) const {
  if (dir.empty() || dir == "/") {
    return IOStatus::OK();
  }
  size_t start = 0;
  if (dir[0] == '/') {
    start = 1;
  }
  while (start < dir.size()) {
    const size_t slash = dir.find('/', start);
    const size_t len = (slash == std::string::npos) ? dir.size() : slash;
    std::string part = dir.substr(0, len);
    if (!part.empty()) {
      IOStatus s = target()->CreateDirIfMissing(part, IOOptions(), dbg);
      if (!s.ok()) {
        return s;
      }
    }
    if (slash == std::string::npos) {
      break;
    }
    start = slash + 1;
  }
  return IOStatus::OK();
}

IOStatus SimulatedHybridFileSystem::EnsureParentDir(
    const std::string& path, IODebugContext* dbg) const {
  if (path.empty()) {
    return IOStatus::OK();
  }
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return IOStatus::OK();
  }
  if (slash == 0) {
    return IOStatus::OK();
  }
  return EnsureDirRecursive(path.substr(0, slash), dbg);
}

IOStatus SimulatedHybridFileSystem::NewRandomAccessFile(
    const std::string& fname, const FileOptions& file_opts,
    std::unique_ptr<FSRandomAccessFile>* result, IODebugContext* dbg) {
  bool should_simulate = false;
  if (ShouldSimulatePath(fname)) {
    if (model_options_.use_xp_model || model_options_.use_dimm_model ||
        is_full_fs_warm_) {
      should_simulate = true;
    } else {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (warm_file_set_.find(fname) != warm_file_set_.end()) {
        should_simulate = true;
      }
    }
  }

  const std::string real_path = ResolveReadPath(fname);
  const bool is_tmpfs = real_path != fname;
  if (stats_ != nullptr && real_path != fname) {
    stats_->tmpfs_read_opens.fetch_add(1);
  }
  FileOptions open_opts = file_opts;
  if (IsTmpfsRedirectEnabled() && IsUnderTmpfsRoot(real_path)) {
    // tmpfs generally does not support O_DIRECT; opening a direct-read file
    // may fail with EINVAL. Disable direct IO for tmpfs paths only.
    open_opts.use_direct_reads = false;
    open_opts.use_direct_writes = false;
  }
  const uint64_t start_us = NowMicros();
  IOStatus s = target()->NewRandomAccessFile(real_path, open_opts, result, dbg);
  const uint64_t end_us = NowMicros();
  if (latency_monitor_ != nullptr) {
    latency_monitor_->Record(SimFsMonitorOp::kOpen, is_tmpfs, /*bytes=*/0,
                             end_us >= start_us ? end_us - start_us : 0);
  }
  if (s.ok() && should_simulate) {
    uint32_t xp_file_id = 0;
    uint32_t dimm_file_id = 0;
    const char* mmap_base = nullptr;
    size_t mmap_len = 0;
    int mmap_fd = -1;
    if (model_options_.use_xp_model) {
      const uintptr_t instance_id = GetInstanceId(stats_);
      std::lock_guard<std::mutex> lk(g_xp_mu);
      XPControllerState& state = g_xp_states[instance_id];
      xp_file_id = GetXpFileId(&state, fname);
    }
    if (model_options_.use_dimm_model) {
      const uintptr_t instance_id = GetInstanceId(stats_);
      std::lock_guard<std::mutex> lk(g_dimm_mu);
      DimmControllerState& state = g_dimm_states[instance_id];
      dimm_file_id = GetDimmFileId(&state, fname);
    }

    // Best-effort: mmap the file when bypassing base IO (virtual-time mode) or
    // when mmap-base-IO is enabled (DAX-like mode). This allows reads to be
    // served from mapped memory and reduces syscall/FS overhead. Falls back to
    // base IO when mmap is unavailable.
    if ((model_options_.use_xp_model || model_options_.use_dimm_model) &&
        (model_options_.xp_bypass_base_io || model_options_.xp_mmap_base_io)) {
      const int fd = ::open(real_path.c_str(), O_RDONLY | O_CLOEXEC);
      if (fd >= 0) {
        struct stat st;
        if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
          void* p = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ,
                           MAP_PRIVATE, fd, /*offset=*/0);
          if (p != MAP_FAILED) {
            mmap_base = reinterpret_cast<const char*>(p);
            mmap_len = static_cast<size_t>(st.st_size);
            mmap_fd = fd;
          } else {
            ::close(fd);
          }
        } else {
          ::close(fd);
        }
      }
    }
    result->reset(new SimulatedHybridRaf(std::move(*result), rate_limiter_,
                                         fname, is_tmpfs, should_simulate,
                                         xp_file_id, dimm_file_id, mmap_base,
                                         mmap_len, mmap_fd,
                                         model_options_, stats_,
                                         latency_monitor_));
  }
  return s;
}

IOStatus SimulatedHybridFileSystem::NewSequentialFile(
    const std::string& fname, const FileOptions& file_opts,
    std::unique_ptr<FSSequentialFile>* result, IODebugContext* dbg) {
  const std::string real_path = ResolveReadPath(fname);
  const bool is_tmpfs = real_path != fname;
  if (stats_ != nullptr && real_path != fname) {
    stats_->tmpfs_read_opens.fetch_add(1);
  }
  FileOptions open_opts = file_opts;
  if (IsTmpfsRedirectEnabled() && IsUnderTmpfsRoot(real_path)) {
    open_opts.use_direct_reads = false;
    open_opts.use_direct_writes = false;
  }
  const uint64_t start_us = NowMicros();
  IOStatus s = target()->NewSequentialFile(real_path, open_opts, result, dbg);
  const uint64_t end_us = NowMicros();
  if (latency_monitor_ != nullptr) {
    latency_monitor_->Record(SimFsMonitorOp::kOpen, is_tmpfs, /*bytes=*/0,
                             end_us >= start_us ? end_us - start_us : 0);
  }
  return s;
}

IOStatus SimulatedHybridFileSystem::NewDirectory(
    const std::string& name, const IOOptions& options,
    std::unique_ptr<FSDirectory>* result, IODebugContext* dbg) {
  const std::string real_path = ResolveWritePath(name);
  IOStatus s = EnsureDirRecursive(real_path, dbg);
  if (!s.ok()) {
    return s;
  }
  return target()->NewDirectory(real_path, options, result, dbg);
}

IOStatus SimulatedHybridFileSystem::NewWritableFile(
    const std::string& fname, const FileOptions& file_opts,
    std::unique_ptr<FSWritableFile>* result, IODebugContext* dbg) {
  if (file_opts.temperature == Temperature::kWarm) {
    const std::lock_guard<std::mutex> lock(mutex_);
    warm_file_set_.insert(fname);
  }

  bool should_simulate = false;
  if (ShouldSimulatePath(fname)) {
    if (model_options_.use_xp_model || model_options_.use_dimm_model) {
      should_simulate = true;
    } else if (file_opts.temperature == Temperature::kWarm || is_full_fs_warm_) {
      should_simulate = true;
    }
  }

  const std::string real_path = ResolveWritePath(fname);
  const bool is_tmpfs = real_path != fname;
  if (stats_ != nullptr && real_path != fname) {
    stats_->tmpfs_write_opens.fetch_add(1);
  }
  IOStatus s = EnsureParentDir(real_path, dbg);
  if (!s.ok()) {
    return s;
  }
  FileOptions open_opts = file_opts;
  if (IsTmpfsRedirectEnabled() && IsUnderTmpfsRoot(real_path)) {
    open_opts.use_direct_reads = false;
    open_opts.use_direct_writes = false;
  }
  const uint64_t start_us = NowMicros();
  s = target()->NewWritableFile(real_path, open_opts, result, dbg);
  const uint64_t end_us = NowMicros();
  if (latency_monitor_ != nullptr) {
    latency_monitor_->Record(SimFsMonitorOp::kOpen, is_tmpfs, /*bytes=*/0,
                             end_us >= start_us ? end_us - start_us : 0);
  }
  if (s.ok() && should_simulate) {
    result->reset(new SimulatedWritableFile(std::move(*result), rate_limiter_,
                                            fname, model_options_, stats_));
  }
  return s;
}

IOStatus SimulatedHybridFileSystem::ReopenWritableFile(
    const std::string& fname, const FileOptions& file_opts,
    std::unique_ptr<FSWritableFile>* result, IODebugContext* dbg) {
  bool should_simulate = false;
  if (ShouldSimulatePath(fname)) {
    if (model_options_.use_xp_model || model_options_.use_dimm_model) {
      should_simulate = true;
    } else if (file_opts.temperature == Temperature::kWarm ||
               is_full_fs_warm_) {
      should_simulate = true;
    } else {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (warm_file_set_.find(fname) != warm_file_set_.end()) {
        should_simulate = true;
      }
    }
  }

  std::string real_path = ResolveReadPath(fname);
  if (real_path == fname) {
    real_path = ResolveWritePath(fname);
  }
  const bool is_tmpfs = real_path != fname;
  if (stats_ != nullptr && real_path != fname) {
    stats_->tmpfs_write_opens.fetch_add(1);
  }
  IOStatus s = EnsureParentDir(real_path, dbg);
  if (!s.ok()) {
    return s;
  }
  FileOptions open_opts = file_opts;
  if (IsTmpfsRedirectEnabled() && IsUnderTmpfsRoot(real_path)) {
    open_opts.use_direct_reads = false;
    open_opts.use_direct_writes = false;
  }
  const uint64_t start_us = NowMicros();
  s = target()->ReopenWritableFile(real_path, open_opts, result, dbg);
  const uint64_t end_us = NowMicros();
  if (latency_monitor_ != nullptr) {
    latency_monitor_->Record(SimFsMonitorOp::kOpen, is_tmpfs, /*bytes=*/0,
                             end_us >= start_us ? end_us - start_us : 0);
  }
  if (s.ok() && should_simulate) {
    result->reset(new SimulatedWritableFile(std::move(*result), rate_limiter_,
                                            fname, model_options_, stats_));
  }
  return s;
}

IOStatus SimulatedHybridFileSystem::ReuseWritableFile(
    const std::string& fname, const std::string& old_fname,
    const FileOptions& file_opts, std::unique_ptr<FSWritableFile>* result,
    IODebugContext* dbg) {
  bool should_simulate = false;
  if (ShouldSimulatePath(fname)) {
    if (model_options_.use_xp_model || model_options_.use_dimm_model) {
      should_simulate = true;
    } else if (file_opts.temperature == Temperature::kWarm ||
               is_full_fs_warm_) {
      should_simulate = true;
    } else {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (warm_file_set_.find(fname) != warm_file_set_.end()) {
        should_simulate = true;
      }
    }
  }

  const std::string old_real = ResolveReadPath(old_fname);
  std::string new_real = ResolveWritePath(fname);
  if (IsTmpfsRedirectEnabled() && IsUnderTmpfsRoot(old_real)) {
    new_real = ToTmpfsPath(fname);
  }
  const bool is_tmpfs = new_real != fname;
  if (stats_ != nullptr && new_real != fname) {
    stats_->tmpfs_write_opens.fetch_add(1);
  }
  IOStatus s = EnsureParentDir(new_real, dbg);
  if (!s.ok()) {
    return s;
  }
  FileOptions open_opts = file_opts;
  if (IsTmpfsRedirectEnabled() && IsUnderTmpfsRoot(new_real)) {
    open_opts.use_direct_reads = false;
    open_opts.use_direct_writes = false;
  }
  const uint64_t start_us = NowMicros();
  s = target()->ReuseWritableFile(new_real, old_real, open_opts, result, dbg);
  const uint64_t end_us = NowMicros();
  if (latency_monitor_ != nullptr) {
    latency_monitor_->Record(SimFsMonitorOp::kOpen, is_tmpfs, /*bytes=*/0,
                             end_us >= start_us ? end_us - start_us : 0);
  }
  if (s.ok() && should_simulate) {
    result->reset(new SimulatedWritableFile(std::move(*result), rate_limiter_,
                                            fname, model_options_, stats_));
  }
  return s;
}

IOStatus SimulatedHybridFileSystem::FileExists(const std::string& fname,
                                               const IOOptions& options,
                                               IODebugContext* dbg) {
  if (IsTmpfsRedirectEnabled() && !IsUnderTmpfsRoot(fname)) {
    const std::string tmp_path = ToTmpfsPath(fname);
    IOStatus s = target()->FileExists(tmp_path, options, dbg);
    if (s.ok()) {
      return s;
    }
    if (!s.IsNotFound()) {
      return s;
    }
  }
  return target()->FileExists(fname, options, dbg);
}

IOStatus SimulatedHybridFileSystem::GetChildren(
    const std::string& dir, const IOOptions& options,
    std::vector<std::string>* result, IODebugContext* dbg) {
  if (IsTmpfsRedirectEnabled() && !IsUnderTmpfsRoot(dir)) {
    const std::string tmp_dir = ToTmpfsPath(dir);
    IOStatus s = target()->GetChildren(tmp_dir, options, result, dbg);
    if (s.ok()) {
      return s;
    }
    if (!s.IsNotFound()) {
      return s;
    }
  }
  return target()->GetChildren(dir, options, result, dbg);
}

IOStatus SimulatedHybridFileSystem::GetChildrenFileAttributes(
    const std::string& dir, const IOOptions& options,
    std::vector<FileAttributes>* result, IODebugContext* dbg) {
  if (IsTmpfsRedirectEnabled() && !IsUnderTmpfsRoot(dir)) {
    const std::string tmp_dir = ToTmpfsPath(dir);
    IOStatus s = target()->GetChildrenFileAttributes(tmp_dir, options, result,
                                                     dbg);
    if (s.ok()) {
      return s;
    }
    if (!s.IsNotFound()) {
      return s;
    }
  }
  return target()->GetChildrenFileAttributes(dir, options, result, dbg);
}

IOStatus SimulatedHybridFileSystem::LockFile(const std::string& fname,
                                             const IOOptions& options,
                                             FileLock** lock,
                                             IODebugContext* dbg) {
  const std::string real_path = ResolveWritePath(fname);
  IOStatus s = EnsureParentDir(real_path, dbg);
  if (!s.ok()) {
    return s;
  }
  FileLock* real_lock = nullptr;
  s = target()->LockFile(real_path, options, &real_lock, dbg);
  if (!s.ok()) {
    return s;
  }
  {
    const std::lock_guard<std::mutex> lock_guard(mutex_);
    lock_map_[real_lock] = real_lock;
  }
  *lock = real_lock;
  return IOStatus::OK();
}

IOStatus SimulatedHybridFileSystem::UnlockFile(FileLock* lock,
                                               const IOOptions& options,
                                               IODebugContext* dbg) {
  FileLock* real_lock = lock;
  {
    const std::lock_guard<std::mutex> lock_guard(mutex_);
    auto it = lock_map_.find(lock);
    if (it != lock_map_.end()) {
      real_lock = it->second;
      lock_map_.erase(it);
    }
  }
  return target()->UnlockFile(real_lock, options, dbg);
}

IOStatus SimulatedHybridFileSystem::CreateDir(const std::string& dirname,
                                              const IOOptions& options,
                                              IODebugContext* dbg) {
  if (IsTmpfsRedirectEnabled() && !IsUnderTmpfsRoot(dirname)) {
    const IOStatus base_status = target()->CreateDirIfMissing(dirname, options, dbg);
    if (!base_status.ok()) {
      return base_status;
    }
  }
  const std::string real_path = ResolveWritePath(dirname);
  return target()->CreateDir(real_path, options, dbg);
}

IOStatus SimulatedHybridFileSystem::CreateDirIfMissing(
    const std::string& dirname, const IOOptions& options,
    IODebugContext* dbg) {
  if (IsTmpfsRedirectEnabled() && !IsUnderTmpfsRoot(dirname)) {
    IOStatus base_status = target()->CreateDirIfMissing(dirname, options, dbg);
    if (!base_status.ok()) {
      return base_status;
    }
  }
  const std::string real_path = ResolveWritePath(dirname);
  return EnsureDirRecursive(real_path, dbg);
}

IOStatus SimulatedHybridFileSystem::DeleteDir(const std::string& dirname,
                                              const IOOptions& options,
                                              IODebugContext* dbg) {
  bool deleted = false;
  IOStatus final_status = IOStatus::OK();

  if (IsTmpfsRedirectEnabled() && !IsUnderTmpfsRoot(dirname)) {
    const std::string tmp_path = ToTmpfsPath(dirname);
    IOStatus s = target()->DeleteDir(tmp_path, options, dbg);
    if (s.ok()) {
      deleted = true;
    } else if (!s.IsNotFound()) {
      final_status = s;
    }
  }

  IOStatus s = target()->DeleteDir(dirname, options, dbg);
  if (s.ok()) {
    deleted = true;
  } else if (!s.IsNotFound()) {
    final_status = s;
  }
  if (deleted || final_status.ok()) {
    return IOStatus::OK();
  }
  return final_status;
}

IOStatus SimulatedHybridFileSystem::GetFileSize(const std::string& fname,
                                                const IOOptions& options,
                                                uint64_t* size,
                                                IODebugContext* dbg) {
  const std::string real_path = ResolveReadPath(fname);
  return target()->GetFileSize(real_path, options, size, dbg);
}

IOStatus SimulatedHybridFileSystem::IsDirectory(const std::string& path,
                                                const IOOptions& options,
                                                bool* is_dir,
                                                IODebugContext* dbg) {
  if (IsTmpfsRedirectEnabled() && !IsUnderTmpfsRoot(path)) {
    const std::string tmp_path = ToTmpfsPath(path);
    IOStatus s = target()->IsDirectory(tmp_path, options, is_dir, dbg);
    if (s.ok()) {
      return s;
    }
    if (!s.IsNotFound()) {
      return s;
    }
  }
  return target()->IsDirectory(path, options, is_dir, dbg);
}

IOStatus SimulatedHybridFileSystem::CopyFileContents(const std::string& src,
                                                     const std::string& dst,
                                                     IODebugContext* dbg) const {
  std::unique_ptr<FSSequentialFile> reader;
  IOStatus s = target()->NewSequentialFile(src, FileOptions(), &reader, dbg);
  if (!s.ok()) {
    return s;
  }

  s = EnsureParentDir(dst, dbg);
  if (!s.ok()) {
    return s;
  }

  std::unique_ptr<FSWritableFile> writer;
  s = target()->NewWritableFile(dst, FileOptions(), &writer, dbg);
  if (!s.ok()) {
    return s;
  }

  constexpr size_t kCopyBufBytes = 4 * 1024 * 1024;
  std::string scratch;
  scratch.resize(kCopyBufBytes);

  const IOOptions io_opts;
  while (true) {
    Slice chunk;
    s = reader->Read(kCopyBufBytes, io_opts, &chunk, scratch.data(), dbg);
    if (!s.ok()) {
      return s;
    }
    if (chunk.empty()) {
      break;
    }
    s = writer->Append(chunk, io_opts, dbg);
    if (!s.ok()) {
      return s;
    }
  }

  s = writer->Sync(io_opts, dbg);
  if (!s.ok()) {
    return s;
  }
  return writer->Close(io_opts, dbg);
}

IOStatus SimulatedHybridFileSystem::RenameFile(const std::string& src,
                                               const std::string& target_path,
                                               const IOOptions& options,
                                               IODebugContext* dbg) {
  std::string src_real = src;
  if (IsTmpfsRedirectEnabled() && !IsUnderTmpfsRoot(src) && TmpfsFileExists(src)) {
    src_real = ToTmpfsPath(src);
  }
  std::string dst_real = target_path;
  if (IsTmpfsRedirectEnabled() && !IsUnderTmpfsRoot(target_path)) {
    if (IsUnderTmpfsRoot(src_real) || ShouldPersistToTmpfs(target_path)) {
      dst_real = ToTmpfsPath(target_path);
    }
  }
  IOStatus s = EnsureParentDir(dst_real, dbg);
  if (!s.ok()) {
    return s;
  }
  s = target()->RenameFile(src_real, dst_real, options, dbg);
  if (s.ok()) {
    return s;
  }

  const std::string err = s.ToString();
  const bool cross_device = err.find("cross-device") != std::string::npos ||
                            err.find("EXDEV") != std::string::npos ||
                            err.find("Invalid cross-device link") !=
                                std::string::npos;
  if (!cross_device) {
    return s;
  }

  IOStatus copy_status = CopyFileContents(src_real, dst_real, dbg);
  if (!copy_status.ok()) {
    return copy_status;
  }
  IOStatus del_status = target()->DeleteFile(src_real, options, dbg);
  if (!del_status.ok() && !del_status.IsNotFound()) {
    return del_status;
  }
  return IOStatus::OK();
}

IOStatus SimulatedHybridFileSystem::NewLogger(const std::string& fname,
                                              const IOOptions& options,
                                              std::shared_ptr<Logger>* result,
                                              IODebugContext* dbg) {
  const std::string real_path = ResolveWritePath(fname);
  IOStatus s = EnsureParentDir(real_path, dbg);
  if (!s.ok()) {
    return s;
  }
  return target()->NewLogger(real_path, options, result, dbg);
}

IOStatus SimulatedHybridFileSystem::DeleteFile(const std::string& fname,
                                               const IOOptions& options,
                                               IODebugContext* dbg) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    warm_file_set_.erase(fname);
  }
  IOStatus final_status = IOStatus::OK();
  bool deleted = false;

  if (IsTmpfsRedirectEnabled() && !IsUnderTmpfsRoot(fname)) {
    const std::string tmp_path = ToTmpfsPath(fname);
    IOStatus s = target()->DeleteFile(tmp_path, options, dbg);
    if (s.ok()) {
      deleted = true;
    } else if (!s.IsNotFound()) {
      final_status = s;
    }
  }

  IOStatus s = target()->DeleteFile(fname, options, dbg);
  if (s.ok()) {
    deleted = true;
  } else if (!s.IsNotFound()) {
    final_status = s;
  }

  if (deleted || final_status.ok()) {
    return IOStatus::OK();
  }
  return final_status;
}

SimulatedHybridRaf::~SimulatedHybridRaf() {
  if (mmap_base_ != nullptr && mmap_len_ > 0) {
    ::munmap(const_cast<char*>(mmap_base_), mmap_len_);
    mmap_base_ = nullptr;
    mmap_len_ = 0;
  }
  if (mmap_fd_ >= 0) {
    ::close(mmap_fd_);
    mmap_fd_ = -1;
  }
}

IOStatus SimulatedHybridRaf::Read(uint64_t offset, size_t n,
                                  const IOOptions& options, Slice* result,
                                  char* scratch, IODebugContext* dbg) const {
  const uint64_t wrapper_start_ns = NowNanos();
  const uint64_t start_us = NowMicros();
  if (should_simulate_) {
    SimulateIOWait(offset, static_cast<uint64_t>(n));
    if ((model_options_.use_xp_model || model_options_.use_dimm_model) &&
        (model_options_.xp_bypass_base_io || model_options_.xp_mmap_base_io)) {
      if (mmap_base_ != nullptr && offset + n <= mmap_len_) {
        // Preserve RocksDB block-cache behavior by returning data in `scratch`
        // when possible. Returning a Slice that points into mmapped memory can
        // cause some callers to treat the data as "already in memory" and skip
        // caching, which is not desirable for the bypass-base-IO judge
        // experiment.
        if (scratch != nullptr) {
          std::memcpy(scratch, mmap_base_ + offset, n);
          *result = Slice(scratch, n);
        } else {
          *result = Slice(mmap_base_ + offset, n);
        }
        const uint64_t end_us = NowMicros();
        const uint64_t wrapper_end_ns = NowNanos();
        if (wrapper_end_ns >= wrapper_start_ns) {
          g_simulated_fs_read_wall_ns += (wrapper_end_ns - wrapper_start_ns);
        }
        if (stats_ != nullptr) {
          if (is_tmpfs_) {
            stats_->tmpfs_read_ops.fetch_add(1);
            stats_->tmpfs_read_bytes.fetch_add(static_cast<uint64_t>(n));
          } else {
            stats_->base_read_ops.fetch_add(1);
            stats_->base_read_bytes.fetch_add(static_cast<uint64_t>(n));
          }
        }
        if (monitor_ != nullptr) {
          monitor_->Record(SimFsMonitorOp::kRead, is_tmpfs_,
                           static_cast<uint64_t>(n),
                           end_us >= start_us ? end_us - start_us : 0);
        }
        return IOStatus::OK();
      }
      // Fallback: mmap unavailable/out-of-range. Do base read to keep data
      // correctness even when mmap-base-IO is requested.
    }
  }
  const uint64_t base_start_ns = NowNanos();
  IOStatus s = target()->Read(offset, n, options, result, scratch, dbg);
  const uint64_t base_end_ns = NowNanos();
  if (base_end_ns >= base_start_ns) {
    g_simulated_fs_base_read_ns += (base_end_ns - base_start_ns);
  }
  const uint64_t end_us = NowMicros();
  const uint64_t wrapper_end_ns = NowNanos();
  if (wrapper_end_ns >= wrapper_start_ns) {
    g_simulated_fs_read_wall_ns += (wrapper_end_ns - wrapper_start_ns);
  }
  if (stats_ != nullptr) {
    if (is_tmpfs_) {
      stats_->tmpfs_read_ops.fetch_add(1);
      stats_->tmpfs_read_bytes.fetch_add(static_cast<uint64_t>(n));
    } else {
      stats_->base_read_ops.fetch_add(1);
      stats_->base_read_bytes.fetch_add(static_cast<uint64_t>(n));
    }
  }
  if (monitor_ != nullptr) {
    monitor_->Record(SimFsMonitorOp::kRead, is_tmpfs_,
                     static_cast<uint64_t>(n),
                     end_us >= start_us ? end_us - start_us : 0);
  }
  return s;
}

IOStatus SimulatedHybridRaf::MultiRead(FSReadRequest* reqs, size_t num_reqs,
                                       const IOOptions& options,
                                       IODebugContext* dbg) {
  const uint64_t wrapper_start_ns = NowNanos();
  const uint64_t start_us = NowMicros();
  uint64_t total_bytes = 0;
  if (should_simulate_) {
    for (size_t i = 0; i < num_reqs; i++) {
      SimulateIOWait(reqs[i].offset, static_cast<uint64_t>(reqs[i].len));
      total_bytes += static_cast<uint64_t>(reqs[i].len);
      if ((model_options_.use_xp_model || model_options_.use_dimm_model) &&
          (model_options_.xp_bypass_base_io || model_options_.xp_mmap_base_io)) {
        if (mmap_base_ == nullptr ||
            reqs[i].offset + reqs[i].len > mmap_len_) {
          // Mmap path unavailable/out-of-range: fall back to base MultiRead.
          goto base_multiread;
        }
        if (reqs[i].scratch != nullptr) {
          std::memcpy(reqs[i].scratch, mmap_base_ + reqs[i].offset, reqs[i].len);
          reqs[i].result = Slice(reqs[i].scratch, reqs[i].len);
        } else {
          reqs[i].result = Slice(mmap_base_ + reqs[i].offset, reqs[i].len);
        }
        reqs[i].status = IOStatus::OK();
      }
    }
    if ((model_options_.use_xp_model || model_options_.use_dimm_model) &&
        (model_options_.xp_bypass_base_io || model_options_.xp_mmap_base_io)) {
      const uint64_t end_us = NowMicros();
      const uint64_t wrapper_end_ns = NowNanos();
      if (wrapper_end_ns >= wrapper_start_ns) {
        g_simulated_fs_read_wall_ns += (wrapper_end_ns - wrapper_start_ns);
      }
      if (stats_ != nullptr) {
        if (is_tmpfs_) {
          stats_->tmpfs_read_ops.fetch_add(num_reqs);
          stats_->tmpfs_read_bytes.fetch_add(total_bytes);
        } else {
          stats_->base_read_ops.fetch_add(num_reqs);
          stats_->base_read_bytes.fetch_add(total_bytes);
        }
      }
      if (monitor_ != nullptr) {
        monitor_->Record(SimFsMonitorOp::kRead, is_tmpfs_, total_bytes,
                         end_us >= start_us ? end_us - start_us : 0);
      }
      return IOStatus::OK();
    }
  }
base_multiread:
  const uint64_t base_start_ns = NowNanos();
  IOStatus s = target()->MultiRead(reqs, num_reqs, options, dbg);
  const uint64_t base_end_ns = NowNanos();
  if (base_end_ns >= base_start_ns) {
    g_simulated_fs_base_read_ns += (base_end_ns - base_start_ns);
  }
  // Best-effort bytes accounting for non-simulated path.
  if (total_bytes == 0) {
    for (size_t i = 0; i < num_reqs; ++i) {
      total_bytes += static_cast<uint64_t>(reqs[i].len);
    }
  }
  const uint64_t end_us = NowMicros();
  const uint64_t wrapper_end_ns = NowNanos();
  if (wrapper_end_ns >= wrapper_start_ns) {
    g_simulated_fs_read_wall_ns += (wrapper_end_ns - wrapper_start_ns);
  }
  if (stats_ != nullptr) {
    if (is_tmpfs_) {
      stats_->tmpfs_read_ops.fetch_add(num_reqs);
      stats_->tmpfs_read_bytes.fetch_add(total_bytes);
    } else {
      stats_->base_read_ops.fetch_add(num_reqs);
      stats_->base_read_bytes.fetch_add(total_bytes);
    }
  }
  if (monitor_ != nullptr) {
    monitor_->Record(SimFsMonitorOp::kRead, is_tmpfs_, total_bytes,
                     end_us >= start_us ? end_us - start_us : 0);
  }
  return s;
}

IOStatus SimulatedHybridRaf::Prefetch(uint64_t offset, size_t n,
                                      const IOOptions& options,
                                      IODebugContext* dbg) {
  const uint64_t wrapper_start_ns = NowNanos();
  const uint64_t start_us = NowMicros();
  if (should_simulate_) {
    SimulateIOWait(offset, static_cast<uint64_t>(n));
    if ((model_options_.use_xp_model || model_options_.use_dimm_model) &&
        model_options_.xp_bypass_base_io) {
      const uint64_t end_us = NowMicros();
      const uint64_t wrapper_end_ns = NowNanos();
      if (wrapper_end_ns >= wrapper_start_ns) {
        g_simulated_fs_read_wall_ns += (wrapper_end_ns - wrapper_start_ns);
      }
      if (stats_ != nullptr) {
        if (is_tmpfs_) {
          stats_->tmpfs_prefetch_ops.fetch_add(1);
          stats_->tmpfs_prefetch_bytes.fetch_add(static_cast<uint64_t>(n));
        } else {
          stats_->base_prefetch_ops.fetch_add(1);
          stats_->base_prefetch_bytes.fetch_add(static_cast<uint64_t>(n));
        }
      }
      if (monitor_ != nullptr) {
        monitor_->Record(SimFsMonitorOp::kPrefetch, is_tmpfs_,
                         static_cast<uint64_t>(n),
                         end_us >= start_us ? end_us - start_us : 0);
      }
      return IOStatus::OK();
    }
  }
  IOStatus s = target()->Prefetch(offset, n, options, dbg);
  const uint64_t end_us = NowMicros();
  const uint64_t wrapper_end_ns = NowNanos();
  if (wrapper_end_ns >= wrapper_start_ns) {
    g_simulated_fs_read_wall_ns += (wrapper_end_ns - wrapper_start_ns);
  }
  if (stats_ != nullptr) {
    if (is_tmpfs_) {
      stats_->tmpfs_prefetch_ops.fetch_add(1);
      stats_->tmpfs_prefetch_bytes.fetch_add(static_cast<uint64_t>(n));
    } else {
      stats_->base_prefetch_ops.fetch_add(1);
      stats_->base_prefetch_bytes.fetch_add(static_cast<uint64_t>(n));
    }
  }
  if (monitor_ != nullptr) {
    monitor_->Record(SimFsMonitorOp::kPrefetch, is_tmpfs_,
                     static_cast<uint64_t>(n),
                     end_us >= start_us ? end_us - start_us : 0);
  }
  return s;
}

void SimulatedHybridRaf::SimulateIOWait(uint64_t offset,
                                        uint64_t logical_bytes) const {
  if (logical_bytes == 0) {
    return;
  }

  if (model_options_.use_dimm_model) {
    SimulateDimmReadQueueServe(dimm_file_id_, offset, logical_bytes,
                               model_options_, stats_);
    return;
  }

  if (model_options_.use_xp_model) {
    SimulateXpReadQueueServe(xp_file_id_, offset, logical_bytes, model_options_,
                             stats_);
    return;
  }

  const uint64_t media_bytes = GetMediaBytes(model_options_, logical_bytes);
  int serve_time_us = CalculateServeTimeUs(static_cast<size_t>(logical_bytes));
  const uint64_t delay_ns = static_cast<uint64_t>(serve_time_us) * 1000;
  StopWatchNano stop_watch(Env::Default()->GetSystemClock().get(),
                           /*auto_start=*/true);
  RateLimiterRequest(rate_limiter_.get(), serve_time_us);
  int time_passed_us = static_cast<int>(stop_watch.ElapsedNanos() / 1000);
  if (time_passed_us < serve_time_us) {
    Env::Default()->SleepForMicroseconds(serve_time_us - time_passed_us);
  }
  if (stats_ != nullptr) {
    stats_->logical_read_bytes.fetch_add(logical_bytes);
    stats_->media_read_bytes.fetch_add(media_bytes);
    stats_->read_ops.fetch_add(1);
    stats_->simulated_read_delay_ns.fetch_add(delay_ns);
    stats_->simulated_read_media_delay_ns.fetch_add(delay_ns);
  }
}

void SimulatedWritableFile::SimulateIOWait(uint64_t logical_bytes,
                                           uint64_t media_bytes) const {
  int serve_time_us = CalculateServeTimeUs(static_cast<size_t>(logical_bytes));
  const uint64_t delay_ns = static_cast<uint64_t>(serve_time_us) * 1000;
  Env::Default()->SleepForMicroseconds(serve_time_us);
  RateLimiterRequest(rate_limiter_.get(), serve_time_us);
  if (stats_ != nullptr) {
    stats_->logical_write_bytes.fetch_add(logical_bytes);
    stats_->media_write_bytes.fetch_add(media_bytes);
    stats_->write_ops.fetch_add(1);
    stats_->simulated_write_delay_ns.fetch_add(delay_ns);
    stats_->simulated_write_media_delay_ns.fetch_add(delay_ns);
  }
}

void SimulatedWritableFile::SimulateXPWrite(uint64_t offset,
                                            uint64_t logical_bytes) const {
  SimulateXpWriteQueueSubmission(file_name_, offset, logical_bytes,
                                 model_options_, stats_);
}

void SimulatedWritableFile::SimulateDimmWrite(uint64_t offset,
                                              uint64_t logical_bytes) const {
  SimulateDimmWriteQueueServe(file_name_, offset, logical_bytes, model_options_,
                              stats_);
}

IOStatus SimulatedWritableFile::Append(const Slice& data, const IOOptions& ioo,
                                       IODebugContext* idc) {
  if (model_options_.use_dimm_model) {
    const uint64_t logical = static_cast<uint64_t>(data.size());
    SimulateDimmWrite(append_file_offset_, logical);
    append_file_offset_ = GetSaturatedEnd(append_file_offset_, logical);
  } else if (model_options_.use_xp_model) {
    const uint64_t logical = static_cast<uint64_t>(data.size());
    SimulateXPWrite(append_file_offset_, logical);
    append_file_offset_ = GetSaturatedEnd(append_file_offset_, logical);
  } else if (use_direct_io()) {
    const uint64_t logical = static_cast<uint64_t>(data.size());
    SimulateIOWait(logical, logical);
  } else {
    unsynced_bytes += data.size();
  }
  return target()->Append(data, ioo, idc);
}

IOStatus SimulatedWritableFile::Append(
    const Slice& data, const IOOptions& options,
    const DataVerificationInfo& verification_info, IODebugContext* dbg) {
  if (model_options_.use_dimm_model) {
    const uint64_t logical = static_cast<uint64_t>(data.size());
    SimulateDimmWrite(append_file_offset_, logical);
    append_file_offset_ = GetSaturatedEnd(append_file_offset_, logical);
  } else if (model_options_.use_xp_model) {
    const uint64_t logical = static_cast<uint64_t>(data.size());
    SimulateXPWrite(append_file_offset_, logical);
    append_file_offset_ = GetSaturatedEnd(append_file_offset_, logical);
  } else if (use_direct_io()) {
    const uint64_t logical = static_cast<uint64_t>(data.size());
    SimulateIOWait(logical, logical);
  } else {
    unsynced_bytes += data.size();
  }
  return target()->Append(data, options, verification_info, dbg);
}

IOStatus SimulatedWritableFile::PositionedAppend(const Slice& data,
                                                 uint64_t offset,
                                                 const IOOptions& options,
                                                 IODebugContext* dbg) {
  if (model_options_.use_dimm_model) {
    const uint64_t logical = static_cast<uint64_t>(data.size());
    SimulateDimmWrite(offset, logical);
    append_file_offset_ =
        std::max<uint64_t>(append_file_offset_, GetSaturatedEnd(offset, logical));
  } else if (model_options_.use_xp_model) {
    const uint64_t logical = static_cast<uint64_t>(data.size());
    SimulateXPWrite(offset, logical);
    append_file_offset_ =
        std::max<uint64_t>(append_file_offset_, GetSaturatedEnd(offset, logical));
  } else if (use_direct_io()) {
    const uint64_t logical = static_cast<uint64_t>(data.size());
    SimulateIOWait(logical, logical);
  } else {
    // This might be overcalculated, but it's probably OK.
    unsynced_bytes += data.size();
  }
  return target()->PositionedAppend(data, offset, options, dbg);
}
IOStatus SimulatedWritableFile::PositionedAppend(
    const Slice& data, uint64_t offset, const IOOptions& options,
    const DataVerificationInfo& verification_info, IODebugContext* dbg) {
  if (model_options_.use_dimm_model) {
    const uint64_t logical = static_cast<uint64_t>(data.size());
    SimulateDimmWrite(offset, logical);
    append_file_offset_ =
        std::max<uint64_t>(append_file_offset_, GetSaturatedEnd(offset, logical));
  } else if (model_options_.use_xp_model) {
    const uint64_t logical = static_cast<uint64_t>(data.size());
    SimulateXPWrite(offset, logical);
    append_file_offset_ =
        std::max<uint64_t>(append_file_offset_, GetSaturatedEnd(offset, logical));
  } else if (use_direct_io()) {
    const uint64_t logical = static_cast<uint64_t>(data.size());
    SimulateIOWait(logical, logical);
  } else {
    // This might be overcalculated, but it's probably OK.
    unsynced_bytes += data.size();
  }
  return target()->PositionedAppend(data, offset, options, verification_info,
                                    dbg);
}

IOStatus SimulatedWritableFile::Sync(const IOOptions& options,
                                     IODebugContext* dbg) {
  if (!model_options_.use_xp_model && unsynced_bytes > 0) {
    const uint64_t logical = static_cast<uint64_t>(unsynced_bytes);
    SimulateIOWait(logical, logical);
    unsynced_bytes = 0;
  }
  return target()->Sync(options, dbg);
}

IOStatus SimulatedWritableFile::Close(const IOOptions& options,
                                      IODebugContext* dbg) {
  return target()->Close(options, dbg);
}
}  // namespace ROCKSDB_NAMESPACE
