// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "rocksdb/env.h"
#include "rocksdb/file_system.h"
#include "rocksdb/slice.h"
#include "tools/nvm_fs/simulated_hybrid_file_system.h"

namespace ROCKSDB_NAMESPACE {

namespace {

struct ToolOptions {
  std::string case_id = "envfs_case";
  std::string work_dir = "/tmp";
  std::string stats_file = "/tmp/envfs_case.kv";
  std::string op = "read";      // read | write
  std::string pattern = "rand"; // seq | rand
  std::string nvm_model = "xp"; // xp | dimm
  uint64_t threads = 1;
  uint64_t value_size = 256;
  uint64_t num_ops = 1000;
  uint64_t file_size = 64ULL * 1024ULL * 1024ULL;
  uint64_t seed = 20260207;

  uint64_t xp_line_bytes = 256;
  uint64_t xp_buffer_bytes = 16ULL * 1024ULL;
  uint64_t xp_latency_ns = 305;
  uint64_t xp_rpq_depth = 64;
  uint64_t xp_wpq_depth = 64;
  uint64_t xp_rpq_parallelism = 1;
  uint64_t xp_wpq_parallelism = 1;
  uint64_t xp_read_line_parallelism = 1;
  uint64_t xp_write_line_parallelism = 1;
  uint64_t xp_rpq_arb_ns = 0;
  uint64_t xp_wpq_submit_ns = 90;
  uint64_t xp_prefetch_hit_ns = 169;
  uint64_t xp_dram_seq_read_ns = 81;
  uint64_t xp_dram_rand_read_ns = 101;
  bool xp_enable_prefetch = true;
  bool xp_share_buffer_between_rw = true;
  bool xp_bypass_base_io = true;
  uint64_t xp_forced_tag_init_stagger_ns = 32;
  bool deterministic_schedule = false;
  uint64_t deterministic_chunk_ops = 1;
  std::string xp_path_prefix;

  // DIMM model knobs (used when --nvm_model=dimm).
  uint64_t dimm_fixed_read_overhead_ns = 1080;
  uint64_t dimm_fixed_write_overhead_ns = 0;
  double dimm_seq_read_bw_gbps = 6.6;
  double dimm_seq_write_bw_gbps = 2.3;
  double dimm_rand_bw_scale = 0.95;
  double dimm_sub_line_random_media_amp = 5.0;
};

struct ToolResult {
  bool ok = true;
  std::string error;
  uint64_t elapsed_ns = 0;
  uint64_t full_elapsed_ns = 0;
  uint64_t total_ops = 0;
  uint64_t slots = 0;
};

struct SimStats {
  double logical_read = 0.0;
  double logical_write = 0.0;
  double media_read = 0.0;
  double media_write = 0.0;
  double read_ops = 0.0;
  double write_ops = 0.0;
  double read_delay_ns = 0.0;
  double write_delay_ns = 0.0;
  double read_queue_delay_ns = 0.0;
  double write_queue_delay_ns = 0.0;
  double read_media_delay_ns = 0.0;
  double read_arb_delay_ns = 0.0;
  double write_media_delay_ns = 0.0;
  double read_virtual_start_ns = 0.0;
  double read_virtual_end_ns = 0.0;
  double write_virtual_start_ns = 0.0;
  double write_virtual_end_ns = 0.0;
};

std::string JoinPath(const std::string& a, const std::string& b) {
  if (a.empty()) {
    return b;
  }
  if (a.back() == '/') {
    return a + b;
  }
  return a + "/" + b;
}

bool ParseBool(std::string v, bool* out) {
  if (out == nullptr) {
    return false;
  }
  for (auto& c : v) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  if (v == "1" || v == "true" || v == "yes" || v == "on") {
    *out = true;
    return true;
  }
  if (v == "0" || v == "false" || v == "no" || v == "off") {
    *out = false;
    return true;
  }
  return false;
}

bool ParseUInt64(const std::string& v, uint64_t* out) {
  if (out == nullptr || v.empty()) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  unsigned long long parsed = std::strtoull(v.c_str(), &end, 10);
  if (errno != 0 || end == v.c_str() || *end != '\0') {
    return false;
  }
  *out = static_cast<uint64_t>(parsed);
  return true;
}

bool ParseDouble(const std::string& v, double* out) {
  if (out == nullptr || v.empty()) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  double parsed = std::strtod(v.c_str(), &end);
  if (errno != 0 || end == v.c_str() || *end != '\0') {
    return false;
  }
  *out = parsed;
  return true;
}

bool ParseArgs(int argc, char** argv, ToolOptions* opts, std::string* err) {
  if (opts == nullptr) {
    if (err != nullptr) {
      *err = "internal error: null options";
    }
    return false;
  }
  auto bad = [&](const std::string& m) {
    if (err != nullptr) {
      *err = m;
    }
    return false;
  };

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind("--", 0) != 0) {
      return bad("invalid argument: " + arg);
    }
    arg = arg.substr(2);
    size_t eq = arg.find('=');
    std::string key = arg.substr(0, eq);
    std::string val = eq == std::string::npos ? "1" : arg.substr(eq + 1);

    if (key == "case_id") {
      opts->case_id = val;
    } else if (key == "work_dir") {
      opts->work_dir = val;
    } else if (key == "stats_file") {
      opts->stats_file = val;
    } else if (key == "op") {
      opts->op = val;
    } else if (key == "pattern") {
      opts->pattern = val;
    } else if (key == "nvm_model") {
      opts->nvm_model = val;
    } else if (key == "threads") {
      if (!ParseUInt64(val, &opts->threads)) return bad("invalid --threads");
    } else if (key == "value_size") {
      if (!ParseUInt64(val, &opts->value_size)) return bad("invalid --value_size");
    } else if (key == "num_ops") {
      if (!ParseUInt64(val, &opts->num_ops)) return bad("invalid --num_ops");
    } else if (key == "file_size") {
      if (!ParseUInt64(val, &opts->file_size)) return bad("invalid --file_size");
    } else if (key == "seed") {
      if (!ParseUInt64(val, &opts->seed)) return bad("invalid --seed");
    } else if (key == "xp_line_bytes") {
      if (!ParseUInt64(val, &opts->xp_line_bytes)) return bad("invalid --xp_line_bytes");
    } else if (key == "xp_buffer_bytes") {
      if (!ParseUInt64(val, &opts->xp_buffer_bytes)) return bad("invalid --xp_buffer_bytes");
    } else if (key == "xp_latency_ns") {
      if (!ParseUInt64(val, &opts->xp_latency_ns)) return bad("invalid --xp_latency_ns");
    } else if (key == "xp_rpq_depth") {
      if (!ParseUInt64(val, &opts->xp_rpq_depth)) return bad("invalid --xp_rpq_depth");
    } else if (key == "xp_wpq_depth") {
      if (!ParseUInt64(val, &opts->xp_wpq_depth)) return bad("invalid --xp_wpq_depth");
    } else if (key == "xp_rpq_parallelism") {
      if (!ParseUInt64(val, &opts->xp_rpq_parallelism)) return bad("invalid --xp_rpq_parallelism");
    } else if (key == "xp_wpq_parallelism") {
      if (!ParseUInt64(val, &opts->xp_wpq_parallelism)) return bad("invalid --xp_wpq_parallelism");
    } else if (key == "xp_read_line_parallelism") {
      if (!ParseUInt64(val, &opts->xp_read_line_parallelism)) {
        return bad("invalid --xp_read_line_parallelism");
      }
    } else if (key == "xp_write_line_parallelism") {
      if (!ParseUInt64(val, &opts->xp_write_line_parallelism)) {
        return bad("invalid --xp_write_line_parallelism");
      }
    } else if (key == "xp_rpq_arb_ns") {
      if (!ParseUInt64(val, &opts->xp_rpq_arb_ns)) {
        return bad("invalid --xp_rpq_arb_ns");
      }
    } else if (key == "xp_wpq_submit_ns") {
      if (!ParseUInt64(val, &opts->xp_wpq_submit_ns)) return bad("invalid --xp_wpq_submit_ns");
    } else if (key == "xp_prefetch_hit_ns") {
      if (!ParseUInt64(val, &opts->xp_prefetch_hit_ns)) return bad("invalid --xp_prefetch_hit_ns");
    } else if (key == "xp_dram_seq_read_ns") {
      if (!ParseUInt64(val, &opts->xp_dram_seq_read_ns)) {
        return bad("invalid --xp_dram_seq_read_ns");
      }
    } else if (key == "xp_dram_rand_read_ns") {
      if (!ParseUInt64(val, &opts->xp_dram_rand_read_ns)) {
        return bad("invalid --xp_dram_rand_read_ns");
      }
    } else if (key == "xp_enable_prefetch") {
      if (!ParseBool(val, &opts->xp_enable_prefetch)) return bad("invalid --xp_enable_prefetch");
    } else if (key == "xp_share_buffer_between_rw") {
      if (!ParseBool(val, &opts->xp_share_buffer_between_rw)) {
        return bad("invalid --xp_share_buffer_between_rw");
      }
    } else if (key == "xp_bypass_base_io") {
      if (!ParseBool(val, &opts->xp_bypass_base_io)) return bad("invalid --xp_bypass_base_io");
    } else if (key == "xp_forced_tag_init_stagger_ns") {
      if (!ParseUInt64(val, &opts->xp_forced_tag_init_stagger_ns)) {
        return bad("invalid --xp_forced_tag_init_stagger_ns");
      }
    } else if (key == "deterministic_schedule") {
      if (!ParseBool(val, &opts->deterministic_schedule)) {
        return bad("invalid --deterministic_schedule");
      }
    } else if (key == "deterministic_chunk_ops") {
      if (!ParseUInt64(val, &opts->deterministic_chunk_ops)) {
        return bad("invalid --deterministic_chunk_ops");
      }
    } else if (key == "xp_path_prefix") {
      opts->xp_path_prefix = val;
    } else if (key == "dimm_fixed_read_overhead_ns") {
      if (!ParseUInt64(val, &opts->dimm_fixed_read_overhead_ns)) {
        return bad("invalid --dimm_fixed_read_overhead_ns");
      }
    } else if (key == "dimm_fixed_write_overhead_ns") {
      if (!ParseUInt64(val, &opts->dimm_fixed_write_overhead_ns)) {
        return bad("invalid --dimm_fixed_write_overhead_ns");
      }
    } else if (key == "dimm_seq_read_bw_gbps") {
      if (!ParseDouble(val, &opts->dimm_seq_read_bw_gbps)) {
        return bad("invalid --dimm_seq_read_bw_gbps");
      }
    } else if (key == "dimm_seq_write_bw_gbps") {
      if (!ParseDouble(val, &opts->dimm_seq_write_bw_gbps)) {
        return bad("invalid --dimm_seq_write_bw_gbps");
      }
    } else if (key == "dimm_rand_bw_scale") {
      if (!ParseDouble(val, &opts->dimm_rand_bw_scale)) {
        return bad("invalid --dimm_rand_bw_scale");
      }
    } else if (key == "dimm_sub_line_random_media_amp") {
      if (!ParseDouble(val, &opts->dimm_sub_line_random_media_amp)) {
        return bad("invalid --dimm_sub_line_random_media_amp");
      }
    } else {
      return bad("unknown flag: --" + key);
    }
  }

  if (opts->op != "read" && opts->op != "write") {
    return bad("op must be read or write");
  }
  if (opts->pattern != "seq" && opts->pattern != "rand") {
    return bad("pattern must be seq or rand");
  }
  if (opts->nvm_model != "xp" && opts->nvm_model != "dimm") {
    return bad("nvm_model must be xp or dimm");
  }
  if (opts->nvm_model == "dimm") {
    if (!(opts->dimm_seq_read_bw_gbps > 0.0) ||
        !(opts->dimm_seq_write_bw_gbps > 0.0)) {
      return bad("dimm_seq_*_bw_gbps must be > 0");
    }
    if (!(opts->dimm_rand_bw_scale >= 0.0) || !(opts->dimm_rand_bw_scale <= 1.0)) {
      return bad("dimm_rand_bw_scale must be within [0,1]");
    }
    if (!(opts->dimm_sub_line_random_media_amp >= 1.0)) {
      return bad("dimm_sub_line_random_media_amp must be >= 1");
    }
  }
  opts->threads = std::max<uint64_t>(1, opts->threads);
  opts->value_size = std::max<uint64_t>(1, opts->value_size);
  opts->num_ops = std::max<uint64_t>(1, opts->num_ops);
  opts->file_size = std::max<uint64_t>(opts->value_size, opts->file_size);
  opts->deterministic_chunk_ops = std::max<uint64_t>(1, opts->deterministic_chunk_ops);
  return true;
}

bool EnsureSparseFile(const std::string& path, uint64_t file_size, std::string* err) {
  const int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
  if (fd < 0) {
    if (err != nullptr) {
      *err = "open failed for " + path;
    }
    return false;
  }
  const off_t size = static_cast<off_t>(std::max<uint64_t>(1, file_size));
  const int rc = ::ftruncate(fd, size);
  ::close(fd);
  if (rc != 0) {
    if (err != nullptr) {
      *err = "ftruncate failed for " + path;
    }
    return false;
  }
  return true;
}

std::map<std::string, double> ParseKv(const std::string& path) {
  std::map<std::string, double> out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    const size_t p = line.find('=');
    if (p == std::string::npos) {
      continue;
    }
    const std::string k = line.substr(0, p);
    const std::string v = line.substr(p + 1);
    try {
      out[k] = std::stod(v);
    } catch (...) {
    }
  }
  return out;
}

double GetOrZero(const std::map<std::string, double>& kv, const std::string& k) {
  auto it = kv.find(k);
  if (it == kv.end()) {
    return 0.0;
  }
  return it->second;
}

ToolResult RunWorkload(const ToolOptions& opts,
                       const std::shared_ptr<SimulatedHybridFileSystem>& simfs) {
  ToolResult out;
  out.total_ops = opts.num_ops;
  out.slots = std::max<uint64_t>(1, opts.file_size / opts.value_size);

  Env* env = Env::Default();
  if (!env->FileExists(opts.work_dir).ok()) {
    Status s = env->CreateDirIfMissing(opts.work_dir);
    if (!s.ok()) {
      out.ok = false;
      out.error = "CreateDirIfMissing failed: " + opts.work_dir;
      return out;
    }
  }

  const uint64_t full_begin = env->NowNanos();
  const uint64_t slots = out.slots;
  if (opts.op == "read") {
    const bool per_thread_seq_file = opts.pattern == "seq" && opts.threads > 1;
    if (per_thread_seq_file) {
      for (uint64_t tid = 0; tid < opts.threads; ++tid) {
        const std::string read_file =
            JoinPath(opts.work_dir, opts.case_id + "_read_t" + std::to_string(tid) + ".bin");
        if (!EnsureSparseFile(read_file, opts.file_size, &out.error)) {
          out.ok = false;
          return out;
        }
      }
    } else {
      const std::string read_file = JoinPath(opts.work_dir, opts.case_id + "_read_t0.bin");
      if (!EnsureSparseFile(read_file, opts.file_size, &out.error)) {
        out.ok = false;
        return out;
      }
    }
  }

  std::atomic<bool> has_error{false};
  std::mutex err_mu;
  std::string first_error;
  const std::string payload(static_cast<size_t>(opts.value_size), 'x');
  const IOOptions io_opts;

  const uint64_t base_ops = opts.num_ops / opts.threads;
  const uint64_t extra_ops = opts.num_ops % opts.threads;
  std::vector<uint64_t> ops_for_thread(opts.threads, base_ops);
  for (uint64_t tid = 0; tid < opts.threads; ++tid) {
    if (tid < extra_ops) {
      ++ops_for_thread[tid];
    }
  }

  const uint64_t begin = env->NowNanos();
  if (opts.deterministic_schedule && opts.threads > 1) {
    struct ScopedSimThreadTag {
      explicit ScopedSimThreadTag(uint64_t tag) {
        SetSimulatedFsThreadTagForCurrentThread(tag);
      }
      ~ScopedSimThreadTag() { ClearSimulatedFsThreadTagForCurrentThread(); }
    };

    if (opts.op == "read") {
      const bool per_thread_seq_file = opts.pattern == "seq" && opts.threads > 1;
      std::vector<std::unique_ptr<FSRandomAccessFile>> rfs(opts.threads);
      std::vector<std::string> scratches(
          opts.threads, std::string(static_cast<size_t>(opts.value_size), '\0'));
      std::vector<uint64_t> local_seq_idx(opts.threads, 0);
      std::vector<std::mt19937_64> rngs;
      rngs.reserve(opts.threads);
      std::uniform_int_distribution<uint64_t> dist(0, slots - 1);
      for (uint64_t tid = 0; tid < opts.threads; ++tid) {
        const uint64_t seed =
            opts.seed + tid * 1315423911ULL + 0x9e3779b97f4a7c15ULL;
        rngs.emplace_back(seed);
        const std::string read_file =
            per_thread_seq_file
                ? JoinPath(opts.work_dir,
                           opts.case_id + "_read_t" + std::to_string(tid) + ".bin")
                : JoinPath(opts.work_dir, opts.case_id + "_read_t0.bin");
        IOStatus s =
            simfs->NewRandomAccessFile(read_file, FileOptions(), &rfs[tid], nullptr);
        if (!s.ok()) {
          out.ok = false;
          out.error = s.ToString();
          return out;
        }
      }
      Slice result;
      std::vector<uint64_t> done_ops(opts.threads, 0);
      uint64_t total_done = 0;
      while (total_done < opts.num_ops) {
        for (uint64_t tid = 0; tid < opts.threads; ++tid) {
          if (done_ops[tid] >= ops_for_thread[tid]) {
            continue;
          }
          const uint64_t remaining = ops_for_thread[tid] - done_ops[tid];
          const uint64_t burst =
              std::min<uint64_t>(opts.deterministic_chunk_ops, remaining);
          ScopedSimThreadTag scope(tid + 1);
          for (uint64_t b = 0; b < burst; ++b) {
            uint64_t slot = 0;
            if (opts.pattern == "seq") {
              slot = local_seq_idx[tid] % slots;
              ++local_seq_idx[tid];
            } else {
              slot = dist(rngs[tid]);
            }
            const uint64_t offset = slot * opts.value_size;
            IOStatus s =
                rfs[tid]->Read(offset, static_cast<size_t>(opts.value_size), io_opts,
                               &result, scratches[tid].data(), nullptr);
            if (!s.ok()) {
              out.ok = false;
              out.error = s.ToString();
              return out;
            }
            ++done_ops[tid];
            ++total_done;
          }
        }
      }
    } else {
      std::vector<std::unique_ptr<FSWritableFile>> wfs(opts.threads);
      std::vector<std::mt19937_64> rngs;
      rngs.reserve(opts.threads);
      std::uniform_int_distribution<uint64_t> dist(0, slots - 1);
      for (uint64_t tid = 0; tid < opts.threads; ++tid) {
        const uint64_t seed =
            opts.seed + tid * 1315423911ULL + 0x9e3779b97f4a7c15ULL;
        rngs.emplace_back(seed);
        const std::string write_file = JoinPath(
            opts.work_dir, opts.case_id + "_write_t" + std::to_string(tid) + ".bin");
        IOStatus s =
            simfs->NewWritableFile(write_file, FileOptions(), &wfs[tid], nullptr);
        if (!s.ok()) {
          out.ok = false;
          out.error = s.ToString();
          return out;
        }
      }
      std::vector<uint64_t> done_ops(opts.threads, 0);
      uint64_t total_done = 0;
      while (total_done < opts.num_ops) {
        for (uint64_t tid = 0; tid < opts.threads; ++tid) {
          if (done_ops[tid] >= ops_for_thread[tid]) {
            continue;
          }
          const uint64_t remaining = ops_for_thread[tid] - done_ops[tid];
          const uint64_t burst =
              std::min<uint64_t>(opts.deterministic_chunk_ops, remaining);
          ScopedSimThreadTag scope(tid + 1);
          for (uint64_t b = 0; b < burst; ++b) {
            IOStatus s;
            if (opts.pattern == "seq") {
              s = wfs[tid]->Append(Slice(payload), io_opts, nullptr);
            } else {
              const uint64_t slot = dist(rngs[tid]);
              const uint64_t offset = slot * opts.value_size;
              s = wfs[tid]->PositionedAppend(Slice(payload), offset, io_opts,
                                             nullptr);
            }
            if (!s.ok()) {
              out.ok = false;
              out.error = s.ToString();
              return out;
            }
            ++done_ops[tid];
            ++total_done;
          }
        }
      }
      for (uint64_t tid = 0; tid < opts.threads; ++tid) {
        ScopedSimThreadTag scope(tid + 1);
        IOStatus s = wfs[tid]->Sync(io_opts, nullptr);
        if (s.ok()) {
          s = wfs[tid]->Close(io_opts, nullptr);
        }
        if (!s.ok()) {
          out.ok = false;
          out.error = s.ToString();
          return out;
        }
      }
    }
    const uint64_t end = env->NowNanos();
    const uint64_t full_end = env->NowNanos();
    out.elapsed_ns = end - begin;
    out.full_elapsed_ns = full_end - full_begin;
    return out;
  }

  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(opts.threads));
  for (uint64_t tid = 0; tid < opts.threads; ++tid) {
    workers.emplace_back([&, tid]() {
      const uint64_t thread_ops = ops_for_thread[tid];
      if (thread_ops == 0) {
        return;
      }
      const uint64_t seed = opts.seed + tid * 1315423911ULL + 0x9e3779b97f4a7c15ULL;
      std::mt19937_64 rng(seed);
      std::uniform_int_distribution<uint64_t> dist(0, slots - 1);

      if (opts.op == "read") {
        const bool per_thread_seq_file = opts.pattern == "seq" && opts.threads > 1;
        const std::string read_file =
            per_thread_seq_file
                ? JoinPath(opts.work_dir,
                           opts.case_id + "_read_t" + std::to_string(tid) + ".bin")
                : JoinPath(opts.work_dir, opts.case_id + "_read_t0.bin");
        std::unique_ptr<FSRandomAccessFile> rf;
        IOStatus s = simfs->NewRandomAccessFile(read_file, FileOptions(), &rf, nullptr);
        if (!s.ok()) {
          std::lock_guard<std::mutex> lk(err_mu);
          if (!has_error.exchange(true)) {
            first_error = s.ToString();
          }
          return;
        }
        std::string scratch(static_cast<size_t>(opts.value_size), '\0');
        Slice result;
        uint64_t local_seq_idx = 0;
        for (uint64_t i = 0; i < thread_ops; ++i) {
          uint64_t slot = 0;
          if (opts.pattern == "seq") {
            slot = local_seq_idx % slots;
            ++local_seq_idx;
          } else {
            slot = dist(rng);
          }
          uint64_t offset = slot * opts.value_size;
          s = rf->Read(offset, static_cast<size_t>(opts.value_size), io_opts, &result,
                       scratch.data(), nullptr);
          if (!s.ok()) {
            std::lock_guard<std::mutex> lk(err_mu);
            if (!has_error.exchange(true)) {
              first_error = s.ToString();
            }
            break;
          }
          if (opts.threads > 1) {
            std::this_thread::yield();
          }
        }
      } else {
        const std::string write_file =
            JoinPath(opts.work_dir, opts.case_id + "_write_t" + std::to_string(tid) + ".bin");
        std::unique_ptr<FSWritableFile> wf;
        IOStatus s = simfs->NewWritableFile(write_file, FileOptions(), &wf, nullptr);
        if (!s.ok()) {
          std::lock_guard<std::mutex> lk(err_mu);
          if (!has_error.exchange(true)) {
            first_error = s.ToString();
          }
          return;
        }
        for (uint64_t i = 0; i < thread_ops; ++i) {
          if (opts.pattern == "seq") {
            s = wf->Append(Slice(payload), io_opts, nullptr);
          } else {
            uint64_t slot = dist(rng);
            uint64_t offset = slot * opts.value_size;
            s = wf->PositionedAppend(Slice(payload), offset, io_opts, nullptr);
          }
          if (!s.ok()) {
            std::lock_guard<std::mutex> lk(err_mu);
            if (!has_error.exchange(true)) {
              first_error = s.ToString();
            }
            break;
          }
          if (opts.threads > 1) {
            std::this_thread::yield();
          }
        }
        if (!has_error.load()) {
          s = wf->Sync(io_opts, nullptr);
          if (s.ok()) {
            s = wf->Close(io_opts, nullptr);
          }
          if (!s.ok()) {
            std::lock_guard<std::mutex> lk(err_mu);
            if (!has_error.exchange(true)) {
              first_error = s.ToString();
            }
          }
        }
      }
    });
  }

  for (auto& t : workers) {
    t.join();
  }
  const uint64_t end = env->NowNanos();
  const uint64_t full_end = env->NowNanos();
  out.elapsed_ns = end - begin;
  out.full_elapsed_ns = full_end - full_begin;

  if (has_error.load()) {
    out.ok = false;
    out.error = first_error.empty() ? "unknown worker error" : first_error;
  }
  return out;
}

void PrintBenchLine(const std::string& name, double micros_per_op,
                    double ops_per_sec, double mb_per_sec) {
  std::cout << std::setprecision(6) << name << " : " << micros_per_op
            << " micros/op " << static_cast<uint64_t>(ops_per_sec) << " ops/sec "
            << mb_per_sec << " MB/s\n";
}

}  // namespace

int EnvFSProfileMain(int argc, char** argv) {
  ToolOptions opts;
  std::string parse_err;
  if (!ParseArgs(argc, argv, &opts, &parse_err)) {
    std::cerr << "envfs_profile_tool: " << parse_err << "\n";
    return 1;
  }

  SimulatedStorageModelOptions model;
  model.use_xp_model = opts.nvm_model == "xp";
  model.use_dimm_model = opts.nvm_model == "dimm";
  model.xp_line_bytes = opts.xp_line_bytes;
  model.xp_buffer_bytes = opts.xp_buffer_bytes;
  model.xp_latency_ns = opts.xp_latency_ns;
  model.xp_rpq_depth = opts.xp_rpq_depth;
  model.xp_wpq_depth = opts.xp_wpq_depth;
  model.xp_rpq_parallelism = opts.xp_rpq_parallelism;
  model.xp_wpq_parallelism = opts.xp_wpq_parallelism;
  model.xp_read_line_parallelism = opts.xp_read_line_parallelism;
  model.xp_write_line_parallelism = opts.xp_write_line_parallelism;
  model.xp_rpq_arb_ns = opts.xp_rpq_arb_ns;
  model.xp_wpq_submit_ns = opts.xp_wpq_submit_ns;
  model.xp_prefetch_hit_ns = opts.xp_prefetch_hit_ns;
  model.dram_read_seq_ns = opts.xp_dram_seq_read_ns;
  model.dram_read_rand_ns = opts.xp_dram_rand_read_ns;
  model.xp_enable_prefetch = opts.xp_enable_prefetch;
  model.xp_share_buffer_between_rw = opts.xp_share_buffer_between_rw;
  model.xp_bypass_base_io = opts.xp_bypass_base_io;
  model.xp_forced_tag_init_stagger_ns = opts.xp_forced_tag_init_stagger_ns;
  model.path_prefix = opts.xp_path_prefix;
  model.stats_file = opts.stats_file;
  if (model.use_dimm_model) {
    model.dimm_fixed_read_overhead_ns = opts.dimm_fixed_read_overhead_ns;
    model.dimm_fixed_write_overhead_ns = opts.dimm_fixed_write_overhead_ns;
    model.dimm_seq_read_bw_gbps = opts.dimm_seq_read_bw_gbps;
    model.dimm_seq_write_bw_gbps = opts.dimm_seq_write_bw_gbps;
    model.dimm_rand_bw_scale = opts.dimm_rand_bw_scale;
    model.dimm_sub_line_random_media_amp = opts.dimm_sub_line_random_media_amp;
  }

  const auto base = FileSystem::Default();
  ToolResult run;
  {
    auto simfs = std::make_shared<SimulatedHybridFileSystem>(base, "", 1, false, model);
    run = RunWorkload(opts, simfs);
  }  // Destructor flushes stats to stats_file.

  if (!run.ok) {
    std::cerr << "envfs_profile_tool: workload failed: " << run.error << "\n";
    return 2;
  }

  const auto kv = ParseKv(opts.stats_file);
  SimStats stats;
  stats.logical_read = GetOrZero(kv, "logical_read_bytes");
  stats.logical_write = GetOrZero(kv, "logical_write_bytes");
  stats.media_read = GetOrZero(kv, "media_read_bytes");
  stats.media_write = GetOrZero(kv, "media_write_bytes");
  stats.read_ops = GetOrZero(kv, "read_ops");
  stats.write_ops = GetOrZero(kv, "write_ops");
  stats.read_delay_ns = GetOrZero(kv, "simulated_read_delay_ns");
  stats.write_delay_ns = GetOrZero(kv, "simulated_write_delay_ns");
  stats.read_queue_delay_ns = GetOrZero(kv, "simulated_read_queue_delay_ns");
  stats.write_queue_delay_ns = GetOrZero(kv, "simulated_write_queue_delay_ns");
  stats.read_media_delay_ns = GetOrZero(kv, "simulated_read_media_delay_ns");
  stats.read_arb_delay_ns = GetOrZero(kv, "simulated_read_arb_delay_ns");
  stats.write_media_delay_ns = GetOrZero(kv, "simulated_write_media_delay_ns");
  stats.read_virtual_start_ns = GetOrZero(kv, "simulated_read_virtual_start_ns");
  stats.read_virtual_end_ns = GetOrZero(kv, "simulated_read_virtual_end_ns");
  stats.write_virtual_start_ns = GetOrZero(kv, "simulated_write_virtual_start_ns");
  stats.write_virtual_end_ns = GetOrZero(kv, "simulated_write_virtual_end_ns");

  const double wall_elapsed_ns = static_cast<double>(std::max<uint64_t>(1, run.elapsed_ns));
  const double wall_ops = static_cast<double>(run.total_ops);
  const double wall_micros_per_op = wall_elapsed_ns / 1000.0 / wall_ops;
  const double wall_ops_per_sec = wall_ops * 1e9 / wall_elapsed_ns;
  const double wall_mb_per_sec =
      wall_ops_per_sec * static_cast<double>(opts.value_size) / (1024.0 * 1024.0);

  const bool is_read = opts.op == "read";
  const double sim_ops = std::max<double>(1.0, is_read ? stats.read_ops : stats.write_ops);
  const double sim_logical_bytes =
      std::max<double>(0.0, is_read ? stats.logical_read : stats.logical_write);
  const double vstart = is_read ? stats.read_virtual_start_ns : stats.write_virtual_start_ns;
  const double vend = is_read ? stats.read_virtual_end_ns : stats.write_virtual_end_ns;
  const bool has_virtual_window = vstart > 0.0 && vend > vstart;
  double sim_elapsed_ns = 0.0;
  if (has_virtual_window) {
    sim_elapsed_ns = vend - vstart;
  } else if (is_read) {
    sim_elapsed_ns = stats.read_delay_ns;
  } else {
    // Write visible delay is submit-only; add queue+media for throughput fitting.
    sim_elapsed_ns =
        stats.write_delay_ns + stats.write_queue_delay_ns + stats.write_media_delay_ns;
  }
  sim_elapsed_ns = std::max<double>(1.0, sim_elapsed_ns);
  const double sim_micros_per_op = sim_elapsed_ns / 1000.0 / sim_ops;
  const double sim_ops_per_sec = sim_ops * 1e9 / sim_elapsed_ns;
  const double sim_mb_per_sec = (sim_logical_bytes > 0.0)
                                    ? (sim_logical_bytes / (sim_elapsed_ns / 1e9)) /
                                          (1024.0 * 1024.0)
                                    : (sim_ops_per_sec *
                                       static_cast<double>(opts.value_size) /
                                       (1024.0 * 1024.0));

  PrintBenchLine("envfs_wall", wall_micros_per_op, wall_ops_per_sec, wall_mb_per_sec);
  std::cout << "envfs_meta elapsed_ns=" << run.elapsed_ns
            << " full_elapsed_ns=" << run.full_elapsed_ns
            << " total_ops=" << run.total_ops << " slots=" << run.slots
            << " op=" << opts.op << " pattern=" << opts.pattern
            << " threads=" << opts.threads << " value_size=" << opts.value_size
            << " file_size=" << opts.file_size << "\n";
  PrintBenchLine("envfs_sim", sim_micros_per_op, sim_ops_per_sec, sim_mb_per_sec);
  // Latency view: per-op delay (includes fixed overhead). This complements
  // envfs_sim, which uses the virtual device busy window for throughput fit.
  const double avg_delay_ns =
      is_read
          ? (stats.read_ops > 0.0 ? (stats.read_delay_ns / stats.read_ops) : 0.0)
          : (stats.write_ops > 0.0 ? (stats.write_delay_ns / stats.write_ops) : 0.0);
  if (avg_delay_ns > 0.0) {
    const double lat_ops_per_sec = 1e9 / avg_delay_ns;
    const double lat_mb_per_sec = lat_ops_per_sec *
                                  static_cast<double>(opts.value_size) /
                                  (1024.0 * 1024.0);
    PrintBenchLine("envfs_sim_lat", avg_delay_ns / 1000.0, lat_ops_per_sec,
                  lat_mb_per_sec);
  }
  std::cout << "envfs_stats logical_read=" << stats.logical_read
            << " logical_write=" << stats.logical_write
            << " media_read=" << stats.media_read
            << " media_write=" << stats.media_write
            << " read_ops=" << stats.read_ops
            << " write_ops=" << stats.write_ops << "\n";
  return 0;
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  return ROCKSDB_NAMESPACE::EnvFSProfileMain(argc, argv);
}
