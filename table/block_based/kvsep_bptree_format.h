// Copyright (c) 2026-present, OpenAI.
// This file contains experimental SST format helpers for KV-separation +
// B+Tree-like indexing experiments. It is not part of RocksDB stable ABI.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rocksdb/rocksdb_namespace.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "util/coding.h"

namespace ROCKSDB_NAMESPACE {

// Meta block name (stored in metaindex) for mapping from data-block handle to
// corresponding value-block handle for KV-separation experiments.
inline constexpr const char kKVSepBptreeValueMapBlockName[] =
    "rocksdb.experimental.kvsep_bptree.value_map";

// Table property key indicating this SST uses KV-separation encoding.
inline constexpr const char kKVSepBptreeTablePropertyKey[] =
    "rocksdb.experimental.kvsep_bptree";

// Binary encoding (little endian fixed):
//   - fixed32 version (currently 1)
//   - fixed32 value_block_bytes (hint, can be 0 if unknown)
//   - fixed64 entry_count
//   - repeated entries:
//       fixed64 data_block_offset
//       fixed64 value_block_offset
//       fixed64 value_block_size
struct KVSepBptreeValueMapEntry {
  uint64_t data_block_offset = 0;
  uint64_t value_block_offset = 0;
  uint64_t value_block_size = 0;
};

inline void EncodeKVSepBptreeValueMap(
    const std::vector<KVSepBptreeValueMapEntry>& entries,
    uint32_t value_block_bytes_hint, std::string* out) {
  assert(out != nullptr);
  out->clear();
  out->reserve(4 + 4 + 8 + entries.size() * 24);
  PutFixed32(out, 1u);
  PutFixed32(out, value_block_bytes_hint);
  PutFixed64(out, static_cast<uint64_t>(entries.size()));
  for (const auto& e : entries) {
    PutFixed64(out, e.data_block_offset);
    PutFixed64(out, e.value_block_offset);
    PutFixed64(out, e.value_block_size);
  }
}

inline Status DecodeKVSepBptreeValueMap(
    const Slice& in, std::vector<KVSepBptreeValueMapEntry>* entries_out,
    uint32_t* value_block_bytes_hint_out) {
  if (entries_out == nullptr || value_block_bytes_hint_out == nullptr) {
    return Status::InvalidArgument(
        "DecodeKVSepBptreeValueMap: null output");
  }
  entries_out->clear();
  *value_block_bytes_hint_out = 0;

  const char* p = in.data();
  const char* limit = in.data() + in.size();
  if (limit - p < 4 + 4 + 8) {
    return Status::Corruption("kvsep value map too small");
  }
  uint32_t version = DecodeFixed32(p);
  p += 4;
  if (version != 1u) {
    return Status::Corruption("kvsep value map unknown version");
  }
  uint32_t hint = DecodeFixed32(p);
  p += 4;
  uint64_t count = DecodeFixed64(p);
  p += 8;
  if (count > (static_cast<uint64_t>(limit - p) / 24u)) {
    return Status::Corruption("kvsep value map truncated");
  }
  entries_out->reserve(static_cast<size_t>(count));
  for (uint64_t i = 0; i < count; ++i) {
    if (limit - p < 24) {
      return Status::Corruption("kvsep value map truncated entries");
    }
    KVSepBptreeValueMapEntry e;
    e.data_block_offset = DecodeFixed64(p);
    p += 8;
    e.value_block_offset = DecodeFixed64(p);
    p += 8;
    e.value_block_size = DecodeFixed64(p);
    p += 8;
    entries_out->push_back(e);
  }
  if (p != limit) {
    // Future-proofing: allow trailing bytes only if they are zero padding.
    for (; p != limit; ++p) {
      if (*p != 0) {
        return Status::Corruption("kvsep value map has trailing garbage");
      }
    }
  }
  *value_block_bytes_hint_out = hint;
  return Status::OK();
}

}  // namespace ROCKSDB_NAMESPACE

