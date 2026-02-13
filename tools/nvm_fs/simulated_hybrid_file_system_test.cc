//  Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "tools/nvm_fs/simulated_hybrid_file_system.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>

#include "file/file_util.h"
#include "rocksdb/env.h"
#include "rocksdb/file_system.h"
#include "test_util/testharness.h"
#include "test_util/testutil.h"

namespace ROCKSDB_NAMESPACE {
namespace {

class SimulatedHybridFileSystemTest : public testing::Test {
 public:
  SimulatedHybridFileSystemTest()
      : env_(Env::Default()), base_fs_(FileSystem::Default()) {
    test_dir_ = test::PerThreadDBPath("simulated_hybrid_file_system_test");
  }

  void SetUp() override { ASSERT_OK(env_->CreateDirIfMissing(test_dir_)); }

  void TearDown() override { ASSERT_OK(DestroyDir(env_, test_dir_)); }

 protected:
  std::string Path(const std::string& file_name) const {
    return test_dir_ + "/" + file_name;
  }

  static std::unordered_map<std::string, std::string> ParseKvFile(
      const std::string& file_path) {
    std::unordered_map<std::string, std::string> out;
    std::ifstream in(file_path);
    std::string line;
    while (std::getline(in, line)) {
      size_t split = line.find('=');
      if (split == std::string::npos) {
        continue;
      }
      out[line.substr(0, split)] = line.substr(split + 1);
    }
    return out;
  }

  static uint64_t GetUint64(
      const std::unordered_map<std::string, std::string>& kv,
      const std::string& key) {
    auto it = kv.find(key);
    if (it == kv.end()) {
      return 0;
    }
    return static_cast<uint64_t>(std::stoull(it->second));
  }

  static double GetDouble(
      const std::unordered_map<std::string, std::string>& kv,
      const std::string& key) {
    auto it = kv.find(key);
    if (it == kv.end()) {
      return 0.0;
    }
    return std::stod(it->second);
  }

  Env* env_;
  std::shared_ptr<FileSystem> base_fs_;
  std::string test_dir_;
};

TEST_F(SimulatedHybridFileSystemTest, XPModelStatsCaptureQueueAndPrefetch) {
  SimulatedStorageModelOptions opts;
  opts.use_xp_model = true;
  opts.xp_line_bytes = 256;
  opts.xp_buffer_bytes = 16 * 1024;
  opts.xp_latency_ns = 300;
  opts.xp_wpq_submit_ns = 90;
  opts.xp_prefetch_hit_ns = 120;
  opts.dram_read_seq_ns = 81;
  opts.dram_read_rand_ns = 101;
  opts.xp_share_buffer_between_rw = false;
  opts.stats_file = Path("xp_logic_stats.kv");

  const std::string file_name = Path("xp_logic_data.bin");
  const std::string payload(512, 'x');

  {
    auto simfs = std::make_shared<SimulatedHybridFileSystem>(base_fs_, "", 1,
                                                             false, opts);

    std::unique_ptr<FSWritableFile> wf;
    ASSERT_OK(simfs->NewWritableFile(file_name, FileOptions(), &wf, nullptr));
    ASSERT_OK(wf->Append(Slice(payload), IOOptions(), nullptr));
    ASSERT_OK(wf->Sync(IOOptions(), nullptr));
    ASSERT_OK(wf->Close(IOOptions(), nullptr));
    wf.reset();

    std::unique_ptr<FSRandomAccessFile> rf;
    ASSERT_OK(simfs->NewRandomAccessFile(file_name, FileOptions(), &rf,
                                         nullptr));

    char scratch0[256];
    char scratch1[256];
    Slice r0;
    Slice r1;
    ASSERT_OK(rf->Read(0, 256, IOOptions(), &r0, scratch0, nullptr));
    ASSERT_OK(rf->Read(256, 256, IOOptions(), &r1, scratch1, nullptr));
    ASSERT_EQ(r0.size(), 256U);
    ASSERT_EQ(r1.size(), 256U);
  }

  auto kv = ParseKvFile(opts.stats_file);
  ASSERT_EQ(GetUint64(kv, "logical_write_bytes"), 512U);
  ASSERT_EQ(GetUint64(kv, "media_write_bytes"), 512U);
  ASSERT_EQ(GetUint64(kv, "write_ops"), 1U);
  ASSERT_GE(GetUint64(kv, "simulated_write_delay_ns"), opts.xp_wpq_submit_ns);
  ASSERT_LT(GetUint64(kv, "simulated_write_delay_ns"),
            GetUint64(kv, "simulated_write_media_delay_ns"));

  ASSERT_EQ(GetUint64(kv, "read_ops"), 2U);
  ASSERT_EQ(GetUint64(kv, "read_prefetch_hits"), 1U);
  ASSERT_EQ(GetUint64(kv, "simulated_read_queue_delay_ns"), 0U);
  ASSERT_EQ(GetUint64(kv, "simulated_read_media_delay_ns"), 420U);
  ASSERT_EQ(GetUint64(kv, "simulated_read_dram_delay_ns"), 162U);
  ASSERT_EQ(GetUint64(kv, "simulated_read_delay_ns"), 582U);
}

TEST_F(SimulatedHybridFileSystemTest, ReadWriteRoundTripWithMultiRead) {
  SimulatedStorageModelOptions opts;
  opts.use_xp_model = true;
  opts.stats_file = Path("rw_stats.kv");

  const std::string file_name = Path("rw_data.bin");

  {
    auto simfs = std::make_shared<SimulatedHybridFileSystem>(base_fs_, "", 1,
                                                             false, opts);

    std::unique_ptr<FSWritableFile> wf;
    ASSERT_OK(simfs->NewWritableFile(file_name, FileOptions(), &wf, nullptr));
    ASSERT_OK(wf->Append(Slice("hello"), IOOptions(), nullptr));
    ASSERT_OK(wf->PositionedAppend(Slice("world"), 5, IOOptions(), nullptr));
    ASSERT_OK(wf->Sync(IOOptions(), nullptr));
    ASSERT_OK(wf->Close(IOOptions(), nullptr));
    wf.reset();

    std::unique_ptr<FSRandomAccessFile> rf;
    ASSERT_OK(simfs->NewRandomAccessFile(file_name, FileOptions(), &rf,
                                         nullptr));

    char scratch[16];
    Slice whole;
    ASSERT_OK(rf->Read(0, 10, IOOptions(), &whole, scratch, nullptr));
    ASSERT_EQ(whole.ToString(), "helloworld");

    char left_scratch[5];
    char right_scratch[5];
    FSReadRequest reqs[2] = {};
    reqs[0].offset = 0;
    reqs[0].len = 5;
    reqs[0].scratch = left_scratch;
    reqs[1].offset = 5;
    reqs[1].len = 5;
    reqs[1].scratch = right_scratch;

    ASSERT_OK(rf->MultiRead(reqs, 2, IOOptions(), nullptr));
    ASSERT_OK(reqs[0].status);
    ASSERT_OK(reqs[1].status);
    ASSERT_EQ(reqs[0].result.ToString(), "hello");
    ASSERT_EQ(reqs[1].result.ToString(), "world");
  }
}

TEST_F(SimulatedHybridFileSystemTest, DRAMBaseLatencyCoversSeqAndRandRead) {
  SimulatedStorageModelOptions opts;
  opts.use_xp_model = true;
  opts.xp_line_bytes = 256;
  opts.xp_buffer_bytes = 16 * 1024;
  opts.xp_latency_ns = 300;
  opts.xp_prefetch_hit_ns = 120;
  opts.dram_read_seq_ns = 81;
  opts.dram_read_rand_ns = 101;
  opts.xp_share_buffer_between_rw = false;
  opts.stats_file = Path("dram_seq_rand.kv");

  const std::string file_name = Path("dram_seq_rand.bin");
  const std::string payload(1024, 'd');

  {
    auto simfs = std::make_shared<SimulatedHybridFileSystem>(base_fs_, "", 1,
                                                             false, opts);
    std::unique_ptr<FSWritableFile> wf;
    ASSERT_OK(simfs->NewWritableFile(file_name, FileOptions(), &wf, nullptr));
    ASSERT_OK(wf->Append(Slice(payload), IOOptions(), nullptr));
    ASSERT_OK(wf->Sync(IOOptions(), nullptr));
    ASSERT_OK(wf->Close(IOOptions(), nullptr));
    wf.reset();

    std::unique_ptr<FSRandomAccessFile> rf;
    ASSERT_OK(simfs->NewRandomAccessFile(file_name, FileOptions(), &rf,
                                         nullptr));
    char a[256];
    char b[256];
    char c[256];
    Slice r0;
    Slice r1;
    Slice r2;
    ASSERT_OK(rf->Read(0, 256, IOOptions(), &r0, a, nullptr));     // seq
    ASSERT_OK(rf->Read(256, 256, IOOptions(), &r1, b, nullptr));   // seq+hit
    ASSERT_OK(rf->Read(0, 256, IOOptions(), &r2, c, nullptr));     // rand
    ASSERT_EQ(r0.size(), 256U);
    ASSERT_EQ(r1.size(), 256U);
    ASSERT_EQ(r2.size(), 256U);
  }

  auto kv = ParseKvFile(opts.stats_file);
  ASSERT_EQ(GetUint64(kv, "read_ops"), 3U);
  ASSERT_EQ(GetUint64(kv, "read_prefetch_hits"), 1U);
  ASSERT_EQ(GetUint64(kv, "simulated_read_queue_delay_ns"), 0U);
  ASSERT_EQ(GetUint64(kv, "simulated_read_media_delay_ns"), 720U);
  ASSERT_EQ(GetUint64(kv, "simulated_read_dram_delay_ns"), 263U);
  ASSERT_EQ(GetUint64(kv, "simulated_read_delay_ns"), 983U);
}

TEST_F(SimulatedHybridFileSystemTest, RpqArbitrationDelayAccumulates) {
  SimulatedStorageModelOptions opts;
  opts.use_xp_model = true;
  opts.xp_bypass_base_io = true;
  opts.xp_line_bytes = 256;
  opts.xp_buffer_bytes = 16 * 1024;
  opts.xp_latency_ns = 300;
  opts.xp_rpq_parallelism = 1;
  opts.xp_rpq_arb_ns = 50;
  opts.xp_enable_prefetch = false;
  opts.dram_read_seq_ns = 0;
  opts.dram_read_rand_ns = 0;
  opts.xp_forced_tag_init_stagger_ns = 1;
  opts.stats_file = Path("rpq_arb.kv");

  const std::string file_name = Path("rpq_arb.bin");
  const std::string payload(1024, 'a');

  {
    auto simfs = std::make_shared<SimulatedHybridFileSystem>(base_fs_, "", 1,
                                                             false, opts);
    std::unique_ptr<FSWritableFile> wf;
    ASSERT_OK(simfs->NewWritableFile(file_name, FileOptions(), &wf, nullptr));
    ASSERT_OK(wf->Append(Slice(payload), IOOptions(), nullptr));
    ASSERT_OK(wf->Sync(IOOptions(), nullptr));
    ASSERT_OK(wf->Close(IOOptions(), nullptr));

    std::unique_ptr<FSRandomAccessFile> rf;
    ASSERT_OK(simfs->NewRandomAccessFile(file_name, FileOptions(), &rf,
                                         nullptr));

    char a[256];
    char b[256];
    Slice r0;
    Slice r1;
    SetSimulatedFsThreadTagForCurrentThread(1);
    ASSERT_OK(rf->Read(0, 256, IOOptions(), &r0, a, nullptr));
    SetSimulatedFsThreadTagForCurrentThread(2);
    ASSERT_OK(rf->Read(256, 256, IOOptions(), &r1, b, nullptr));
    ClearSimulatedFsThreadTagForCurrentThread();
    ASSERT_EQ(r0.size(), 256U);
    ASSERT_EQ(r1.size(), 256U);
  }

  auto kv = ParseKvFile(opts.stats_file);
  ASSERT_EQ(GetUint64(kv, "read_ops"), 2U);
  ASSERT_EQ(GetUint64(kv, "simulated_read_media_delay_ns"), 600U);
  ASSERT_EQ(GetUint64(kv, "simulated_read_arb_delay_ns"), 50U);
  ASSERT_EQ(GetUint64(kv, "simulated_read_delay_ns"), 949U);
}

TEST_F(SimulatedHybridFileSystemTest, TargetLevelsOnlySimulateSelectedSst) {
  ClearSimulatedFsFileLevels();
  SimulatedStorageModelOptions opts;
  opts.use_xp_model = true;
  opts.xp_bypass_base_io = true;
  opts.target_levels = {0, 1};
  opts.stats_file = Path("target_levels.kv");

  const std::string l0_file = Path("target_l0.sst");
  const std::string l2_file = Path("target_l2.sst");
  RegisterSimulatedFsFileLevel(l0_file, 0);
  RegisterSimulatedFsFileLevel(l2_file, 2);

  const std::string payload(256, 'x');
  {
    auto simfs = std::make_shared<SimulatedHybridFileSystem>(base_fs_, "", 1,
                                                             false, opts);

    std::unique_ptr<FSWritableFile> wf_l0;
    ASSERT_OK(simfs->NewWritableFile(l0_file, FileOptions(), &wf_l0, nullptr));
    ASSERT_OK(wf_l0->Append(Slice(payload), IOOptions(), nullptr));
    ASSERT_OK(wf_l0->Sync(IOOptions(), nullptr));
    ASSERT_OK(wf_l0->Close(IOOptions(), nullptr));

    std::unique_ptr<FSWritableFile> wf_l2;
    ASSERT_OK(simfs->NewWritableFile(l2_file, FileOptions(), &wf_l2, nullptr));
    ASSERT_OK(wf_l2->Append(Slice(payload), IOOptions(), nullptr));
    ASSERT_OK(wf_l2->Sync(IOOptions(), nullptr));
    ASSERT_OK(wf_l2->Close(IOOptions(), nullptr));

    std::unique_ptr<FSRandomAccessFile> rf_l0;
    ASSERT_OK(simfs->NewRandomAccessFile(l0_file, FileOptions(), &rf_l0,
                                         nullptr));
    std::unique_ptr<FSRandomAccessFile> rf_l2;
    ASSERT_OK(simfs->NewRandomAccessFile(l2_file, FileOptions(), &rf_l2,
                                         nullptr));

    char scratch_l0[256];
    char scratch_l2[256];
    Slice r0;
    Slice r2;
    ASSERT_OK(rf_l0->Read(0, 256, IOOptions(), &r0, scratch_l0, nullptr));
    ASSERT_OK(rf_l2->Read(0, 256, IOOptions(), &r2, scratch_l2, nullptr));
    ASSERT_EQ(r0.size(), 256U);
    ASSERT_EQ(r2.size(), 256U);
  }

  auto kv = ParseKvFile(opts.stats_file);
  ASSERT_EQ(GetUint64(kv, "write_ops"), 1U);
  ASSERT_EQ(GetUint64(kv, "read_ops"), 1U);
  ASSERT_EQ(GetUint64(kv, "logical_write_bytes"), 256U);
  ASSERT_EQ(GetUint64(kv, "logical_read_bytes"), 256U);

  ClearSimulatedFsFileLevels();
}

TEST_F(SimulatedHybridFileSystemTest,
       SharedXPBufferCapacityInterferesReadPrefetch) {
  const std::string read_file = Path("shared_capacity_read.bin");
  const std::string write_file = Path("shared_capacity_write.bin");
  const std::string read_payload(4096, 'r');
  const std::string write_payload(2048, 'w');  // 8 lines when xp_line=256.

  auto RunScenario = [&](bool with_write,
                         const std::string& stats_file) {
    SimulatedStorageModelOptions opts;
    opts.use_xp_model = true;
    opts.xp_line_bytes = 256;
    opts.xp_buffer_bytes = 1024;  // 4 lines shared capacity.
    opts.xp_latency_ns = 300;
    opts.xp_prefetch_hit_ns = 120;
    opts.xp_enable_prefetch = true;
    opts.xp_share_buffer_between_rw = true;
    opts.dram_read_seq_ns = 0;
    opts.dram_read_rand_ns = 0;
    opts.stats_file = stats_file;

    auto simfs = std::make_shared<SimulatedHybridFileSystem>(base_fs_, "", 1,
                                                             false, opts);
    std::unique_ptr<FSWritableFile> seed;
    ASSERT_OK(simfs->NewWritableFile(read_file, FileOptions(), &seed, nullptr));
    ASSERT_OK(seed->Append(Slice(read_payload), IOOptions(), nullptr));
    ASSERT_OK(seed->Sync(IOOptions(), nullptr));
    ASSERT_OK(seed->Close(IOOptions(), nullptr));
    seed.reset();

    std::unique_ptr<FSRandomAccessFile> rf;
    ASSERT_OK(
        simfs->NewRandomAccessFile(read_file, FileOptions(), &rf, nullptr));
    char a[256];
    char b[256];
    Slice r0;
    Slice r1;
    ASSERT_OK(rf->Read(0, 256, IOOptions(), &r0, a, nullptr));

    if (with_write) {
      std::unique_ptr<FSWritableFile> wf;
      ASSERT_OK(
          simfs->NewWritableFile(write_file, FileOptions(), &wf, nullptr));
      ASSERT_OK(wf->Append(Slice(write_payload), IOOptions(), nullptr));
      ASSERT_OK(wf->Sync(IOOptions(), nullptr));
      ASSERT_OK(wf->Close(IOOptions(), nullptr));
    }

    ASSERT_OK(rf->Read(256, 256, IOOptions(), &r1, b, nullptr));
    ASSERT_EQ(r0.size(), 256U);
    ASSERT_EQ(r1.size(), 256U);
  };

  const std::string stats_without_write = Path("shared_capacity_no_write.kv");
  const std::string stats_with_write = Path("shared_capacity_with_write.kv");
  RunScenario(/*with_write=*/false, stats_without_write);
  RunScenario(/*with_write=*/true, stats_with_write);

  auto kv_no_write = ParseKvFile(stats_without_write);
  auto kv_with_write = ParseKvFile(stats_with_write);
  ASSERT_EQ(GetUint64(kv_no_write, "read_ops"), 2U);
  ASSERT_EQ(GetUint64(kv_with_write, "read_ops"), 2U);
  ASSERT_EQ(GetUint64(kv_no_write, "read_prefetch_hits"), 1U);
  ASSERT_EQ(GetUint64(kv_with_write, "read_prefetch_hits"), 0U);
  ASSERT_LT(GetUint64(kv_no_write, "simulated_read_media_delay_ns"),
            GetUint64(kv_with_write, "simulated_read_media_delay_ns"));
}

TEST_F(SimulatedHybridFileSystemTest, SimulatedPowerCyclePreservesData) {
  const std::string metadata_file = Path("powercycle.meta");
  const std::string file_name = Path("powercycle.bin");
  const std::string payload(8192, 'p');

  SimulatedStorageModelOptions opts_before;
  opts_before.use_xp_model = true;
  opts_before.stats_file = Path("power_before.kv");

  {
    auto simfs = std::make_shared<SimulatedHybridFileSystem>(
        base_fs_, metadata_file, 1, false, opts_before);

    std::unique_ptr<FSWritableFile> wf;
    ASSERT_OK(simfs->NewWritableFile(file_name, FileOptions(), &wf, nullptr));
    ASSERT_OK(wf->Append(Slice(payload), IOOptions(), nullptr));
    ASSERT_OK(wf->Sync(IOOptions(), nullptr));
    // Simulate a sudden power cut: no explicit Close(), drop handles directly.
    wf.reset();
  }

  SimulatedStorageModelOptions opts_after;
  opts_after.use_xp_model = true;
  opts_after.stats_file = Path("power_after.kv");

  {
    auto simfs_after = std::make_shared<SimulatedHybridFileSystem>(
        base_fs_, metadata_file, 1, false, opts_after);
    std::string reloaded;
    ASSERT_OK(ReadFileToString(simfs_after.get(), file_name, &reloaded));
    ASSERT_EQ(reloaded, payload);
  }

  auto kv = ParseKvFile(opts_after.stats_file);
  ASSERT_EQ(GetDouble(kv, "ewr"), 0.0);
}

}  // namespace
}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
