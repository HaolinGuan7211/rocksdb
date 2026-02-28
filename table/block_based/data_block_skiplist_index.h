// Copyright (c) 2011-present, Facebook, Inc. All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rocksdb/slice.h"

namespace ROCKSDB_NAMESPACE {

// Experimental: a "skiplist-ish" data-block index intended to reduce CPU cost
// of restart-key decoding/comparisons for large data blocks.
//
// The serialized index is stored as a blob right after the restart array and
// before the (optional) data block hash index.
//
// DATA_BLOCK: [DATA ... | RESTARTS[] | SKIPLIST_IDX | SKIPLIST_SIZE(u16)
//              | (optional HASH_IDX) | FOOTER(u32)]
//
// SKIPLIST_IDX format:
//   [MAGIC(u32) | VERSION(u16) | STRIDE_RESTARTS(u16) | NUM_ENTRIES(u32) |
//    ENTRY[NUM_ENTRIES] | KEYS_BLOB]
//
// ENTRY format (fixed width, little-endian):
//   [RESTART_INDEX(u32) | KEY_OFFSET(u32) | KEY_SIZE(u32)]
//
// KEY_OFFSET is relative to the start of SKIPLIST_IDX.
class DataBlockSkipListIndexBuilder {
 public:
  DataBlockSkipListIndexBuilder() : enabled_(false), stride_restarts_(0) {}

  void Initialize(uint16_t stride_restarts) {
    enabled_ = true;
    stride_restarts_ = stride_restarts;
  }

  inline bool Enabled() const { return enabled_; }
  inline uint16_t StrideRestarts() const { return stride_restarts_; }

  // Builds skiplist index from restart points in `block_data` and writes the
  // serialized blob into `out`. Returns true on success.
  bool Build(const std::string& block_data,
             const std::vector<uint32_t>& restart_offsets,
             bool use_value_delta_encoding, std::string* out) const;

 private:
  bool enabled_;
  uint16_t stride_restarts_;
};

class DataBlockSkipListIndex {
 public:
  DataBlockSkipListIndex()
      : data_(nullptr), size_(0), num_entries_(0), stride_restarts_(0) {}

  void Initialize(const char* data, uint16_t size);

  inline bool Valid() const { return data_ != nullptr && num_entries_ > 0; }
  inline uint32_t NumEntries() const { return num_entries_; }
  inline uint16_t StrideRestarts() const { return stride_restarts_; }

  uint32_t RestartIndex(uint32_t i) const;
  Slice Key(uint32_t i) const;

 private:
  const char* data_;
  uint16_t size_;
  uint32_t num_entries_;
  uint16_t stride_restarts_;
};

}  // namespace ROCKSDB_NAMESPACE

