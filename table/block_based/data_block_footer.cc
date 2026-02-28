//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/block_based/data_block_footer.h"

#include "rocksdb/table.h"

namespace ROCKSDB_NAMESPACE {

// Data block footer packs:
// - lower bits: num_restarts
// - upper bits: data-block index type flags
//
// Historically we used only the MSB (bit 31) as "has hash index" flag.
// We extend this to also use bit 30 as "has skiplist-ish index" flag while
// preserving backward compatibility with existing SST files.
const int kDataBlockHashIndexTypeBitShift = 31;
const int kDataBlockSkipListIndexTypeBitShift = 30;

// 0x3FFFFFFF
const uint32_t kMaxNumRestarts =
    (1u << kDataBlockSkipListIndexTypeBitShift) - 1u;

// 0x3FFFFFFF
const uint32_t kNumRestartsMask =
    (1u << kDataBlockSkipListIndexTypeBitShift) - 1u;

uint32_t PackIndexTypeAndNumRestarts(
    BlockBasedTableOptions::DataBlockIndexType index_type,
    uint32_t num_restarts) {
  if (num_restarts > kMaxNumRestarts) {
    assert(0);  // mute travis "unused" warning
  }

  uint32_t block_footer = num_restarts;
  switch (index_type) {
    case BlockBasedTableOptions::kDataBlockBinarySearch:
      break;
    case BlockBasedTableOptions::kDataBlockBinaryAndHash:
      block_footer |= 1u << kDataBlockHashIndexTypeBitShift;
      break;
    case BlockBasedTableOptions::kDataBlockBinaryAndSkipList:
      block_footer |= 1u << kDataBlockSkipListIndexTypeBitShift;
      break;
    case BlockBasedTableOptions::kDataBlockBinaryAndHashAndSkipList:
      block_footer |= 1u << kDataBlockHashIndexTypeBitShift;
      block_footer |= 1u << kDataBlockSkipListIndexTypeBitShift;
      break;
    default:
      assert(0);
  }

  return block_footer;
}

void UnPackIndexTypeAndNumRestarts(
    uint32_t block_footer,
    BlockBasedTableOptions::DataBlockIndexType* index_type,
    uint32_t* num_restarts) {
  if (index_type) {
    const bool has_hash =
        (block_footer & (1u << kDataBlockHashIndexTypeBitShift)) != 0;
    const bool has_skiplist =
        (block_footer & (1u << kDataBlockSkipListIndexTypeBitShift)) != 0;
    if (has_hash && has_skiplist) {
      *index_type =
          BlockBasedTableOptions::kDataBlockBinaryAndHashAndSkipList;
    } else if (has_hash) {
      *index_type = BlockBasedTableOptions::kDataBlockBinaryAndHash;
    } else if (has_skiplist) {
      *index_type = BlockBasedTableOptions::kDataBlockBinaryAndSkipList;
    } else {
      *index_type = BlockBasedTableOptions::kDataBlockBinarySearch;
    }
  }

  if (num_restarts) {
    *num_restarts = block_footer & kNumRestartsMask;
    assert(*num_restarts <= kMaxNumRestarts);
  }
}

}  // namespace ROCKSDB_NAMESPACE
