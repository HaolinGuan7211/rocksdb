// Experimental: per-SST hash index meta block for point lookup acceleration.
//
// This is not part of RocksDB stable ABI. The on-disk format is versioned and
// intended only for experiments in this fork.
#pragma once

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "rocksdb/rocksdb_namespace.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "util/coding.h"

namespace ROCKSDB_NAMESPACE {

inline constexpr const char kExperimentalSstHashIndexMetaBlockName[] =
    "rocksdb.experimental.sst_hash_index";

// On-disk header (fixed-size, little endian).
//
// Notes:
// - `restart_interval` is recorded for analysis/debugging; the MVP hint uses
//   restart_idx computed as entry_idx_in_block / restart_interval.
// - Hashing is on user keys (bytes), and correctness is enforced by verifying
//   against the data block contents on lookup.
struct ExperimentalSstHashIndexHeaderV1 {
  uint32_t version = 1;
  uint32_t flags = 0;
  uint32_t user_key_fixed_len = 0;
  uint32_t fingerprint_bits = 16;
  uint64_t hash_seed = 0;

  uint32_t restart_interval = 0;
  uint32_t hint_type = 0;  // 0: restart_only, 1: restart+within (reserved)

  uint32_t num_data_blocks = 0;
  uint32_t num_slots = 0;
  uint32_t num_entries = 0;
  uint32_t reserved0 = 0;
};

// Slot encoding is fixed-width to allow mmapped access and simple decoding:
//   - fixed32 block_id (0xFFFFFFFF indicates empty)
//   - fixed32 fp16|restart_idx (low 16 fp, high 16 restart_idx)
//   - fixed32 within|reserved (low 16 within, high 16 reserved)
inline constexpr uint32_t kExperimentalSstHashIndexEmptyBlockId = 0xFFFFFFFFu;

struct ExperimentalSstHashIndexSlotV1 {
  uint32_t block_id = kExperimentalSstHashIndexEmptyBlockId;
  uint16_t fp16 = 0;
  uint16_t restart_idx = 0;
  uint16_t within = 0;
  uint16_t reserved = 0;
};

inline void EncodeExperimentalSstHashIndexHeaderV1(
    const ExperimentalSstHashIndexHeaderV1& h, std::string* out) {
  assert(out != nullptr);
  out->clear();
  out->reserve(4 * 10 + 8);
  PutFixed32(out, h.version);
  PutFixed32(out, h.flags);
  PutFixed32(out, h.user_key_fixed_len);
  PutFixed32(out, h.fingerprint_bits);
  PutFixed64(out, h.hash_seed);
  PutFixed32(out, h.restart_interval);
  PutFixed32(out, h.hint_type);
  PutFixed32(out, h.num_data_blocks);
  PutFixed32(out, h.num_slots);
  PutFixed32(out, h.num_entries);
  PutFixed32(out, h.reserved0);
}

inline Status DecodeExperimentalSstHashIndexHeaderV1(
    const Slice& in, ExperimentalSstHashIndexHeaderV1* out,
    size_t* bytes_consumed) {
  if (out == nullptr || bytes_consumed == nullptr) {
    return Status::InvalidArgument("sst_hash_index header: null output");
  }
  *bytes_consumed = 0;
  if (in.size() < (4u * 10u + 8u)) {
    return Status::Corruption("sst_hash_index header: truncated");
  }
  const char* p = in.data();
  out->version = DecodeFixed32(p);
  p += 4;
  if (out->version != 1u) {
    return Status::Corruption("sst_hash_index header: bad version");
  }
  out->flags = DecodeFixed32(p);
  p += 4;
  out->user_key_fixed_len = DecodeFixed32(p);
  p += 4;
  out->fingerprint_bits = DecodeFixed32(p);
  p += 4;
  out->hash_seed = DecodeFixed64(p);
  p += 8;
  out->restart_interval = DecodeFixed32(p);
  p += 4;
  out->hint_type = DecodeFixed32(p);
  p += 4;
  out->num_data_blocks = DecodeFixed32(p);
  p += 4;
  out->num_slots = DecodeFixed32(p);
  p += 4;
  out->num_entries = DecodeFixed32(p);
  p += 4;
  out->reserved0 = DecodeFixed32(p);
  p += 4;
  *bytes_consumed = static_cast<size_t>(p - in.data());
  return Status::OK();
}

inline void EncodeExperimentalSstHashIndexSlotV1(
    const ExperimentalSstHashIndexSlotV1& s, std::string* out) {
  assert(out != nullptr);
  PutFixed32(out, s.block_id);
  const uint32_t fp_restart =
      (static_cast<uint32_t>(s.restart_idx) << 16) | static_cast<uint32_t>(s.fp16);
  PutFixed32(out, fp_restart);
  const uint32_t within_res =
      (static_cast<uint32_t>(s.reserved) << 16) | static_cast<uint32_t>(s.within);
  PutFixed32(out, within_res);
}

inline ExperimentalSstHashIndexSlotV1 DecodeExperimentalSstHashIndexSlotV1(
    const char* p) {
  ExperimentalSstHashIndexSlotV1 s;
  s.block_id = DecodeFixed32(p);
  const uint32_t fp_restart = DecodeFixed32(p + 4);
  s.fp16 = static_cast<uint16_t>(fp_restart & 0xFFFFu);
  s.restart_idx = static_cast<uint16_t>((fp_restart >> 16) & 0xFFFFu);
  const uint32_t within_res = DecodeFixed32(p + 8);
  s.within = static_cast<uint16_t>(within_res & 0xFFFFu);
  s.reserved = static_cast<uint16_t>((within_res >> 16) & 0xFFFFu);
  return s;
}

// BlockDir entry encoding:
//   - fixed64 block_offset
//   - fixed64 block_size
inline void EncodeExperimentalSstHashIndexBlockDirEntry(uint64_t offset,
                                                        uint64_t size,
                                                        std::string* out) {
  assert(out != nullptr);
  PutFixed64(out, offset);
  PutFixed64(out, size);
}

inline Status DecodeExperimentalSstHashIndexBlockDirEntry(
    const char* p, const char* limit, uint64_t* offset_out, uint64_t* size_out) {
  if (offset_out == nullptr || size_out == nullptr) {
    return Status::InvalidArgument("sst_hash_index blockdir: null output");
  }
  if (limit - p < 16) {
    return Status::Corruption("sst_hash_index blockdir: truncated");
  }
  *offset_out = DecodeFixed64(p);
  *size_out = DecodeFixed64(p + 8);
  return Status::OK();
}

}  // namespace ROCKSDB_NAMESPACE
