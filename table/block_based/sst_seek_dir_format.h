// Experimental: per-SST seek directory meta block for iterator Seek().
//
// This is not part of RocksDB stable ABI. The on-disk format is versioned and
// intended only for experiments in this fork.
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

#include "rocksdb/rocksdb_namespace.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "util/coding.h"

namespace ROCKSDB_NAMESPACE {

inline constexpr const char kExperimentalSstSeekDirMetaBlockName[] =
    "rocksdb.experimental.sst_seek_dir";

// Header.flags bits.
// When set, the meta block uses a fixed-len layout without offsets:
//   [header][keys_blob], where keys_blob has (num_data_blocks * user_key_fixed_len) bytes.
inline constexpr uint32_t kExperimentalSstSeekDirFlagNoOffsets = 1u << 0;

// On-disk header (fixed-size, little endian).
//
// Layout of the meta block:
//   [header bytes]
//   [fixed32 key_offsets[num_data_blocks + 1]]
//   [keys_blob bytes]
//
// Each data block i has its (last) user key stored at:
//   keys_blob[ key_offsets[i] : key_offsets[i+1] )
//
// Seek() can binary-search these per-block boundaries by user key to quickly
// identify the candidate data block id for the first key >= target. This is
// designed for Seek-heavy workloads where "point hash lookup" is a poor match.
struct ExperimentalSstSeekDirHeaderV1 {
  uint32_t version = 1;
  uint32_t flags = 0;
  uint32_t num_data_blocks = 0;
  uint32_t user_key_fixed_len = 0;  // 0 means variable-length.
};

inline void EncodeExperimentalSstSeekDirHeaderV1(
    const ExperimentalSstSeekDirHeaderV1& h, std::string* out) {
  assert(out != nullptr);
  out->clear();
  out->reserve(16);
  PutFixed32(out, h.version);
  PutFixed32(out, h.flags);
  PutFixed32(out, h.num_data_blocks);
  PutFixed32(out, h.user_key_fixed_len);
}

inline Status DecodeExperimentalSstSeekDirHeaderV1(
    const Slice& in, ExperimentalSstSeekDirHeaderV1* out,
    size_t* bytes_consumed) {
  if (out == nullptr || bytes_consumed == nullptr) {
    return Status::InvalidArgument("sst_seek_dir header: null output");
  }
  *bytes_consumed = 0;
  if (in.size() < 16u) {
    return Status::Corruption("sst_seek_dir header: truncated");
  }
  const char* p = in.data();
  out->version = DecodeFixed32(p);
  p += 4;
  if (out->version != 1u) {
    return Status::Corruption("sst_seek_dir header: bad version");
  }
  out->flags = DecodeFixed32(p);
  p += 4;
  out->num_data_blocks = DecodeFixed32(p);
  p += 4;
  out->user_key_fixed_len = DecodeFixed32(p);
  p += 4;
  *bytes_consumed = static_cast<size_t>(p - in.data());
  return Status::OK();
}

}  // namespace ROCKSDB_NAMESPACE
