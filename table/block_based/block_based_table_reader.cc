//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
#include "table/block_based/block_based_table_reader.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "block_cache.h"
#include "cache/cache_entry_roles.h"
#include "cache/cache_key.h"
#include "db/compaction/compaction_picker.h"
#include "db/dbformat.h"
#include "db/pinned_iterators_manager.h"
#include "file/file_prefetch_buffer.h"
#include "file/file_util.h"
#include "file/random_access_file_reader.h"
#include "logging/logging.h"
#include "monitoring/perf_context_imp.h"
#include "parsed_full_filter_block.h"
#include "port/lang.h"
#include "rocksdb/cache.h"
#include "rocksdb/comparator.h"
#include "rocksdb/convenience.h"
#include "rocksdb/env.h"
#include "table/block_based/block_prefetcher.h"
#include "rocksdb/file_system.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/iterator.h"
#include "rocksdb/options.h"
#include "rocksdb/snapshot.h"
#include "rocksdb/statistics.h"
#include "rocksdb/system_clock.h"
#include "rocksdb/table.h"
#include "rocksdb/table_properties.h"
#include "rocksdb/trace_record.h"
#include "rocksdb/user_defined_index.h"
#include "table/block_based/binary_search_index_reader.h"
#include "table/block_based/block.h"
#include "table/block_based/block_based_table_factory.h"
#include "table/block_based/block_based_table_iterator.h"
#include "table/block_based/block_prefix_index.h"
#include "table/block_based/block_type.h"
#include "table/block_based/filter_block.h"
#include "table/block_based/filter_policy_internal.h"
#include "table/block_based/full_filter_block.h"
#include "table/block_based/hash_index_reader.h"
#include "table/block_based/partitioned_filter_block.h"
#include "table/block_based/partitioned_index_reader.h"
#include "table/block_based/kvsep_bptree_format.h"
#include "table/block_based/user_defined_index_wrapper.h"
#include "table/block_fetcher.h"
#include "table/format.h"
#include "table/get_context.h"
#include "table/internal_iterator.h"
#include "table/meta_blocks.h"
#include "table/multiget_context.h"
#include "table/persistent_cache_helper.h"
#include "table/persistent_cache_options.h"
#include "table/sst_file_writer_collectors.h"
#include "table/two_level_iterator.h"
#include "test_util/sync_point.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/stop_watch.h"
#include "util/string_util.h"

namespace ROCKSDB_NAMESPACE {
namespace {

CacheAllocationPtr CopyBufferToHeap(MemoryAllocator* allocator, Slice& buf) {
  CacheAllocationPtr heap_buf;
  heap_buf = AllocateBlock(buf.size(), allocator);
  memcpy(heap_buf.get(), buf.data(), buf.size());
  return heap_buf;
}

struct KVSepPinnedHandleKey {
  uint64_t offset = 0;
  uint64_t size = 0;
};

struct KVSepPinnedHandleKeyHash {
  size_t operator()(const KVSepPinnedHandleKey& k) const noexcept {
    // A simple 128->64 bit mix; correctness does not depend on hash quality.
    const uint64_t x = k.offset ^ (k.offset >> 33) ^ (k.size << 1) ^
                       (k.size >> 31);
    return static_cast<size_t>(x) ^ static_cast<size_t>(x >> 32);
  }
};

inline bool operator==(const KVSepPinnedHandleKey& a,
                       const KVSepPinnedHandleKey& b) noexcept {
  return a.offset == b.offset && a.size == b.size;
}

inline void CreateExperimentalKVSepPrefetchBufferIfNeeded(
    const BlockBasedTable::Rep* rep, const ReadOptions& ro,
    std::unique_ptr<FilePrefetchBuffer>* fpb, size_t fallback_readahead_bytes,
    FilePrefetchBufferUsage usage, bool allow_implicit_readahead = true) {
  if (rep == nullptr || fpb == nullptr || *fpb) {
    return;
  }
  if (ro.read_tier == ReadTier::kBlockCacheTier) {
    // IO disallowed; no need to allocate prefetch state.
    return;
  }
  if (rep->ioptions.allow_mmap_reads) {
    // FilePrefetchBuffer doesn't work in mmap mode and readahead isn't needed.
    return;
  }

  size_t bytes = ro.readahead_size;
  if (allow_implicit_readahead) {
    if (bytes == 0) {
      bytes = rep->table_options.initial_auto_readahead_size;
    }
    if (bytes == 0) {
      bytes = fallback_readahead_bytes;
    }
  }
  if (bytes == 0) {
    return;
  }

  ReadaheadParams readahead_params;
  readahead_params.initial_readahead_size = bytes;
  readahead_params.max_readahead_size = bytes;
  rep->CreateFilePrefetchBuffer(readahead_params, fpb,
                                /*readaheadsize_cb=*/nullptr, usage);
}

template <typename TBlocklike>
inline void RecordExperimentalKVSepBlockCacheHit(Statistics* stats) {
  if (stats == nullptr) {
    return;
  }
  if constexpr (std::is_same_v<TBlocklike, Block_kKVSepLeaf>) {
    RecordTick(stats, EXPERIMENTAL_KVSEP_BPTREE_LEAF_CACHE_HIT, 1);
  } else if constexpr (std::is_same_v<TBlocklike, Block_kKVSepValue>) {
    RecordTick(stats, EXPERIMENTAL_KVSEP_BPTREE_VALUE_CACHE_HIT, 1);
  } else if constexpr (std::is_same_v<TBlocklike, Block_kKVSepPair>) {
    RecordTick(stats, EXPERIMENTAL_KVSEP_BPTREE_PAIR_CACHE_HIT, 1);
  }
}

template <typename TBlocklike>
inline void RecordExperimentalKVSepBlockCacheMiss(Statistics* stats) {
  if (stats == nullptr) {
    return;
  }
  if constexpr (std::is_same_v<TBlocklike, Block_kKVSepLeaf>) {
    RecordTick(stats, EXPERIMENTAL_KVSEP_BPTREE_LEAF_CACHE_MISS, 1);
  } else if constexpr (std::is_same_v<TBlocklike, Block_kKVSepValue>) {
    RecordTick(stats, EXPERIMENTAL_KVSEP_BPTREE_VALUE_CACHE_MISS, 1);
  } else if constexpr (std::is_same_v<TBlocklike, Block_kKVSepPair>) {
    RecordTick(stats, EXPERIMENTAL_KVSEP_BPTREE_PAIR_CACHE_MISS, 1);
  }
}

template <typename TBlocklike>
inline void RecordExperimentalKVSepBlockFileRead(Statistics* stats,
                                                const BlockHandle& handle) {
  if (stats == nullptr) {
    return;
  }
  const uint64_t bytes = BlockBasedTable::BlockSizeWithTrailer(handle);
  if constexpr (std::is_same_v<TBlocklike, Block_kKVSepLeaf>) {
    RecordTick(stats, EXPERIMENTAL_KVSEP_BPTREE_LEAF_FILE_READS, 1);
    RecordTick(stats, EXPERIMENTAL_KVSEP_BPTREE_LEAF_FILE_READ_BYTES, bytes);
  } else if constexpr (std::is_same_v<TBlocklike, Block_kKVSepValue>) {
    RecordTick(stats, EXPERIMENTAL_KVSEP_BPTREE_VALUE_FILE_READS, 1);
    RecordTick(stats, EXPERIMENTAL_KVSEP_BPTREE_VALUE_FILE_READ_BYTES, bytes);
  } else if constexpr (std::is_same_v<TBlocklike, Block_kKVSepPair>) {
    RecordTick(stats, EXPERIMENTAL_KVSEP_BPTREE_PAIR_FILE_READS, 1);
    RecordTick(stats, EXPERIMENTAL_KVSEP_BPTREE_PAIR_FILE_READ_BYTES, bytes);
  }
}

}  // namespace

class KVSepBptreeIndexReader;

Block* KVSepBptreeGetPinnedIndexBlock(const KVSepBptreeIndexReader* reader,
                                      const BlockHandle& handle);

class KVSepBptreeIndexIterator final : public InternalIteratorBase<IndexValue> {
 public:
  KVSepBptreeIndexIterator(const BlockBasedTable* table,
                           const ReadOptions& read_options,
                           uint32_t index_levels, TableReaderCaller caller,
                           const KVSepBptreeIndexReader* index_reader,
                           const BlockHandle& pinned_root_handle,
                           Block* pinned_root_block)
      : table_(table),
        read_options_(read_options),
        index_levels_(index_levels),
        lookup_context_(caller),
        index_reader_(index_reader),
        pinned_root_handle_(pinned_root_handle),
        pinned_root_block_(pinned_root_block) {
    iters_.resize(index_levels_);
    for (auto& it : iters_) {
      it.reset(new IndexBlockIter());
    }
    points_to_real_block_.assign(index_levels_, false);
    prev_block_offset_.assign(index_levels_, std::numeric_limits<uint64_t>::max());
  }

  void Seek(const Slice& target) override { SeekImpl(&target); }
  void SeekToFirst() override { SeekImpl(nullptr); }

  void SeekToLast() override {
    ResetBelowLevel(/*level=*/index_levels_ - 1);
    LoadIndexBlockAtLevel(index_levels_ - 1, table_->get_rep()->index_handle);
    if (!iters_[index_levels_ - 1]->status().ok()) {
      return;
    }
    iters_[index_levels_ - 1]->SeekToLast();
    if (!iters_[index_levels_ - 1]->Valid()) {
      ResetBelowLevel(index_levels_ - 1);
      return;
    }
    DescendToLast(index_levels_ - 2);
  }

  void Next() override {
    if (!Valid()) {
      return;
    }
    iters_[0]->Next();
    FindKeyForward();
  }

  void Prev() override {
    if (!Valid()) {
      return;
    }
    iters_[0]->Prev();
    FindKeyBackward();
  }

  void SeekForPrev(const Slice&) override {
    // Not needed for our current experimental workloads.
    assert(false);
  }

  bool Valid() const override {
    return points_to_real_block_[0] && iters_[0] != nullptr &&
           iters_[0]->Valid();
  }

  Slice key() const override {
    assert(Valid());
    return iters_[0]->key();
  }

  Slice user_key() const override {
    assert(Valid());
    return iters_[0]->user_key();
  }

  IndexValue value() const override {
    assert(Valid());
    return iters_[0]->value();
  }

  Status status() const override {
    // Prefer surfacing the first non-OK status from any loaded level.
    for (uint32_t level = 0; level < index_levels_; ++level) {
      if (!points_to_real_block_[level]) {
        continue;
      }
      Status s = iters_[level]->status();
      if (!s.ok() && !s.IsNotFound()) {
        return s;
      }
    }
    return Status::OK();
  }

  IterBoundCheck UpperBoundCheckResult() override {
    return IterBoundCheck::kUnknown;
  }
  void SetPinnedItersMgr(PinnedIteratorsManager*) override { assert(false); }
  bool IsKeyPinned() const override { return false; }
  bool IsValuePinned() const override { return false; }

 private:
 const BlockBasedTable* table_;
  const ReadOptions read_options_;
  const uint32_t index_levels_;
  BlockCacheLookupContext lookup_context_;
  const KVSepBptreeIndexReader* const index_reader_;
  const BlockHandle pinned_root_handle_;
  Block* pinned_root_block_;
  std::vector<std::unique_ptr<IndexBlockIter>> iters_;
  std::vector<bool> points_to_real_block_;
  std::vector<uint64_t> prev_block_offset_;
  std::unique_ptr<FilePrefetchBuffer> prefetch_buffer_;

  FilePrefetchBuffer* GetOrInitPrefetchBuffer() {
    CreateExperimentalKVSepPrefetchBufferIfNeeded(
        table_->get_rep(), read_options_, &prefetch_buffer_,
        /*fallback_readahead_bytes=*/16 * 1024,
        FilePrefetchBufferUsage::kUnknown);
    return prefetch_buffer_.get();
  }

  void SeekImpl(const Slice* target) {
    ResetBelowLevel(/*level=*/index_levels_ - 1);
    LoadIndexBlockAtLevel(index_levels_ - 1, table_->get_rep()->index_handle);
    if (!iters_[index_levels_ - 1]->status().ok()) {
      return;
    }

    if (target) {
      iters_[index_levels_ - 1]->Seek(*target);
    } else {
      iters_[index_levels_ - 1]->SeekToFirst();
    }
    if (!iters_[index_levels_ - 1]->Valid()) {
      ResetBelowLevel(index_levels_ - 1);
      return;
    }

    if (index_levels_ == 1) {
      // Root already points to data blocks.
      points_to_real_block_[0] = points_to_real_block_[index_levels_ - 1];
      return;
    }

    // Descend to leaf.
    for (uint32_t level = index_levels_ - 1; level > 0; --level) {
      const BlockHandle child = iters_[level]->value().handle;
      LoadIndexBlockAtLevel(level - 1, child);
      if (!iters_[level - 1]->status().ok()) {
        return;
      }
      if (target) {
        iters_[level - 1]->Seek(*target);
      } else {
        iters_[level - 1]->SeekToFirst();
      }
      if (!iters_[level - 1]->Valid()) {
        // If child is unexpectedly empty, behave as invalid.
        ResetBelowLevel(level - 1);
        return;
      }
    }
  }

  void ResetBelowLevel(uint32_t level) {
    for (uint32_t l = 0; l < level; ++l) {
      if (points_to_real_block_[l]) {
        iters_[l]->Invalidate(Status::OK());
        points_to_real_block_[l] = false;
      }
      prev_block_offset_[l] = std::numeric_limits<uint64_t>::max();
    }
  }

  void LoadIndexBlockAtLevel(uint32_t level, const BlockHandle& handle) {
    if (points_to_real_block_[level] &&
        handle.offset() == prev_block_offset_[level] &&
        !iters_[level]->status().IsIncomplete()) {
      return;
    }
    if (points_to_real_block_[level]) {
      iters_[level]->Invalidate(Status::OK());
      points_to_real_block_[level] = false;
    }

    // Fast path: for the B+tree root, pin it during table open so we don't
    // re-read it from the file for every lookup when block cache is disabled.
    // This is analogous to the standard BlockBasedTable index reader keeping
    // the root index block resident.
    const bool is_root_level = (level == index_levels_ - 1);
    if (is_root_level && pinned_root_block_ != nullptr &&
        handle.offset() == pinned_root_handle_.offset() &&
        handle.size() == pinned_root_handle_.size()) {
      const auto* rep = table_->get_rep();
      // Always use total-order seek for correctness; this is an internal index.
      pinned_root_block_->NewIndexIterator(
          rep->internal_comparator.user_comparator(),
          rep->get_global_seqno(BlockType::kIndex), iters_[level].get(),
          rep->ioptions.stats, /*total_order_seek=*/true,
          rep->index_has_first_key, rep->index_key_includes_seq,
          rep->index_value_is_full,
          /*block_contents_pinned=*/true,
          rep->user_defined_timestamps_persisted);
      points_to_real_block_[level] = true;
      prev_block_offset_[level] = handle.offset();
      return;
    }

    if (index_reader_ != nullptr) {
      Block* pinned = KVSepBptreeGetPinnedIndexBlock(index_reader_, handle);
      if (pinned != nullptr) {
        const auto* rep = table_->get_rep();
        pinned->NewIndexIterator(
            rep->internal_comparator.user_comparator(),
            rep->get_global_seqno(BlockType::kIndex), iters_[level].get(),
            rep->ioptions.stats, /*total_order_seek=*/true,
            rep->index_has_first_key, rep->index_key_includes_seq,
            rep->index_value_is_full,
            /*block_contents_pinned=*/true,
            rep->user_defined_timestamps_persisted);
        points_to_real_block_[level] = true;
        prev_block_offset_[level] = handle.offset();
        return;
      }
    }

    Status s;
    table_->NewDataBlockIterator<IndexBlockIter>(
        read_options_, handle, iters_[level].get(), BlockType::kIndex,
        /*get_context=*/nullptr, &lookup_context_,
        /*prefetch_buffer=*/GetOrInitPrefetchBuffer(),
        /*for_compaction=*/false, /*async_read=*/false, s,
        /*use_block_cache_for_lookup=*/true);
    points_to_real_block_[level] = true;
    prev_block_offset_[level] = handle.offset();
  }

  void DescendToFirst(int32_t start_level) {
    for (int32_t level = start_level; level >= 0; --level) {
      const BlockHandle child = iters_[level + 1]->value().handle;
      LoadIndexBlockAtLevel(static_cast<uint32_t>(level), child);
      if (!iters_[level]->status().ok()) {
        return;
      }
      iters_[level]->SeekToFirst();
      if (!iters_[level]->Valid()) {
        return;
      }
    }
  }

  void DescendToLast(int32_t start_level) {
    for (int32_t level = start_level; level >= 0; --level) {
      const BlockHandle child = iters_[level + 1]->value().handle;
      LoadIndexBlockAtLevel(static_cast<uint32_t>(level), child);
      if (!iters_[level]->status().ok()) {
        return;
      }
      iters_[level]->SeekToLast();
      if (!iters_[level]->Valid()) {
        return;
      }
    }
  }

  void FindKeyForward() {
    if (iters_[0]->Valid()) {
      return;
    }
    // Advance up, then descend to first.
    for (uint32_t level = 1; level < index_levels_; ++level) {
      if (!points_to_real_block_[level]) {
        break;
      }
      if (!iters_[level - 1]->status().ok()) {
        return;
      }
      iters_[level]->Next();
      if (!iters_[level]->Valid()) {
        continue;
      }
      DescendToFirst(static_cast<int32_t>(level) - 1);
      return;
    }
    // Past the end: invalidate leaf.
    iters_[0]->Invalidate(Status::OK());
  }

  void FindKeyBackward() {
    while (!iters_[0]->Valid()) {
      if (!iters_[0]->status().ok()) {
        return;
      }
      // Move to previous child at some upper level.
      bool moved = false;
      for (uint32_t level = 1; level < index_levels_; ++level) {
        if (!points_to_real_block_[level]) {
          break;
        }
        iters_[level]->Prev();
        if (iters_[level]->Valid()) {
          DescendToLast(static_cast<int32_t>(level) - 1);
          moved = true;
          break;
        }
      }
      if (!moved) {
        return;
      }
    }
  }
};

class KVSepBptreeIndexReader final : public BlockBasedTable::IndexReader {
 public:
  static Status Create(const BlockBasedTable* table, const ReadOptions& ro,
                       FilePrefetchBuffer* prefetch_buffer, bool use_cache,
                       bool prefetch, bool /*pin*/,
                       BlockCacheLookupContext* lookup_context,
                       std::unique_ptr<IndexReader>* index_reader) {
    assert(table != nullptr);
    assert(index_reader != nullptr);
    (void)ro;
    (void)prefetch_buffer;
    (void)use_cache;
    (void)prefetch;
    (void)lookup_context;
    // Keep the B+tree root index block resident (pinned by the IndexReader).
    // Without this, cache=0 runs end up re-reading the root for every lookup,
    // which dominates on NVM-latency scales and makes comparisons unfair vs.
    // the standard index reader (which keeps the root block in memory).
    CachableEntry<Block_kIndex> root_block;
    Status s = table->RetrieveBlock<Block_kIndex>(
        prefetch_buffer, ro, table->get_rep()->index_handle,
        table->get_rep()->decompressor.get(), &root_block,
        /*get_context=*/nullptr, lookup_context, /*for_compaction=*/false,
        /*use_cache=*/use_cache,
        /*async_read=*/false,
        /*use_block_cache_for_lookup=*/true);
    if (!s.ok()) {
      return s;
    }
    if (root_block.GetValue() == nullptr) {
      return Status::Corruption("kvsep bptree: missing root index block");
    }

    std::unordered_map<KVSepPinnedHandleKey, CachableEntry<Block_kIndex>,
                       KVSepPinnedHandleKeyHash>
        pinned_index_blocks;
    uint64_t pinned_index_bytes = 0;
    const uint32_t index_levels = table->get_rep()->kvsep_bptree_index_levels;
    // If block cache is disabled, multi-level B+tree index lookups can become
    // dominated by repeated reads of internal index blocks. Preload and pin the
    // entire index tree once during table open to bring the per-Seek IO count
    // closer to the standard index reader behavior (root is already pinned).
    const auto* const block_cache = table->get_rep()->table_options.block_cache.get();
    const bool cache_disabled =
        (!use_cache) || (block_cache == nullptr) || (block_cache->GetCapacity() == 0);
    const bool pin_index_tree = cache_disabled && index_levels > 1;
    if (pin_index_tree) {
      std::vector<BlockHandle> cur;
      cur.push_back(table->get_rep()->index_handle);
      std::unordered_set<KVSepPinnedHandleKey, KVSepPinnedHandleKeyHash> visited;
      visited.reserve(1024);
      visited.insert(
          KVSepPinnedHandleKey{table->get_rep()->index_handle.offset(),
                               table->get_rep()->index_handle.size()});

      // Walk levels from root down to level 1, collecting children.
      // `cur` will end up holding the level-0 index blocks, which directly
      // point to KV-sep leaf/pair blocks.
      for (uint32_t level = index_levels - 1; level > 0; --level) {
        std::vector<BlockHandle> next;
        next.reserve(cur.size() * 2);
        for (const auto& h : cur) {
          Block* b = nullptr;
          if (h.offset() == table->get_rep()->index_handle.offset() &&
              h.size() == table->get_rep()->index_handle.size()) {
            b = root_block.GetValue();
          } else {
            const KVSepPinnedHandleKey k{h.offset(), h.size()};
            auto it = pinned_index_blocks.find(k);
            if (it == pinned_index_blocks.end()) {
              CachableEntry<Block_kIndex> loaded;
              Status ls = table->RetrieveBlock<Block_kIndex>(
                  prefetch_buffer, ro, h, table->get_rep()->decompressor.get(),
                  &loaded, /*get_context=*/nullptr, lookup_context,
                  /*for_compaction=*/false, /*use_cache=*/false,
                  /*async_read=*/false, /*use_block_cache_for_lookup=*/true);
              if (!ls.ok()) {
                return ls;
              }
              if (loaded.GetValue() == nullptr) {
                return Status::Corruption(
                    "kvsep bptree: missing internal index block");
              }
              pinned_index_bytes += loaded.GetValue()->ApproximateMemoryUsage();
              it = pinned_index_blocks.emplace(k, std::move(loaded)).first;
            }
            b = it->second.GetValue();
          }
          if (b == nullptr) {
            return Status::Corruption("kvsep bptree: null pinned index block");
          }

          IndexBlockIter tmp;
          const auto* rep = table->get_rep();
          b->NewIndexIterator(rep->internal_comparator.user_comparator(),
                              rep->get_global_seqno(BlockType::kIndex), &tmp,
                              rep->ioptions.stats, /*total_order_seek=*/true,
                              rep->index_has_first_key, rep->index_key_includes_seq,
                              rep->index_value_is_full,
                              /*block_contents_pinned=*/true,
                              rep->user_defined_timestamps_persisted);
          for (tmp.SeekToFirst(); tmp.Valid(); tmp.Next()) {
            const BlockHandle child = tmp.value().handle;
            const KVSepPinnedHandleKey ck{child.offset(), child.size()};
            if (visited.insert(ck).second) {
              next.push_back(child);
            }
          }
          Status ts = tmp.status();
          if (!ts.ok() && !ts.IsNotFound()) {
            return ts;
          }
        }
        cur.swap(next);
      }

      // Pin level-0 index blocks as well. These are frequently accessed on the
      // Seek/MultiGet path under cache=0, and leaving them unpinned can dominate
      // lookup latency due to repeated index block I/O.
      for (const auto& h : cur) {
        if (h.offset() == table->get_rep()->index_handle.offset() &&
            h.size() == table->get_rep()->index_handle.size()) {
          continue;
        }
        const KVSepPinnedHandleKey k{h.offset(), h.size()};
        auto it = pinned_index_blocks.find(k);
        if (it != pinned_index_blocks.end()) {
          continue;
        }
        CachableEntry<Block_kIndex> loaded;
        Status ls = table->RetrieveBlock<Block_kIndex>(
            prefetch_buffer, ro, h, table->get_rep()->decompressor.get(),
            &loaded, /*get_context=*/nullptr, lookup_context,
            /*for_compaction=*/false, /*use_cache=*/false,
            /*async_read=*/false, /*use_block_cache_for_lookup=*/true);
        if (!ls.ok()) {
          return ls;
        }
        if (loaded.GetValue() == nullptr) {
          return Status::Corruption("kvsep bptree: missing leaf index block");
        }
        pinned_index_bytes += loaded.GetValue()->ApproximateMemoryUsage();
        pinned_index_blocks.emplace(k, std::move(loaded));
      }
    }

    index_reader->reset(new KVSepBptreeIndexReader(
        table, index_levels, table->get_rep()->index_handle, std::move(root_block),
        std::move(pinned_index_blocks), pinned_index_bytes));
    return Status::OK();
  }

  InternalIteratorBase<IndexValue>* NewIterator(
      const ReadOptions& read_options, bool /*disable_prefix_seek*/,
      IndexBlockIter* /*iter*/, GetContext* /*get_context*/,
      BlockCacheLookupContext* lookup_context) override {
    const auto caller =
        lookup_context ? lookup_context->caller : TableReaderCaller::kUserIterator;
    return new KVSepBptreeIndexIterator(table_, read_options, index_levels_,
                                        caller, this, pinned_root_handle_,
                                        pinned_root_block_.GetValue());
  }

  size_t ApproximateMemoryUsage() const override {
    return sizeof(*this) + static_cast<size_t>(pinned_index_bytes_);
  }

  Block* GetPinnedIndexBlock(const BlockHandle& handle) const {
    if (handle.offset() == pinned_root_handle_.offset() &&
        handle.size() == pinned_root_handle_.size()) {
      return pinned_root_block_.GetValue();
    }
    const KVSepPinnedHandleKey k{handle.offset(), handle.size()};
    auto it = pinned_index_blocks_.find(k);
    if (it == pinned_index_blocks_.end()) {
      return nullptr;
    }
    return it->second.GetValue();
  }

 private:
  KVSepBptreeIndexReader(const BlockBasedTable* table, uint32_t index_levels,
                         const BlockHandle& pinned_root_handle,
                         CachableEntry<Block_kIndex>&& pinned_root_block,
                         std::unordered_map<KVSepPinnedHandleKey,
                                            CachableEntry<Block_kIndex>,
                                            KVSepPinnedHandleKeyHash>&&
                             pinned_index_blocks,
                         uint64_t pinned_index_bytes)
      : table_(table),
        index_levels_(index_levels),
        pinned_root_handle_(pinned_root_handle),
        pinned_root_block_(std::move(pinned_root_block)),
        pinned_index_blocks_(std::move(pinned_index_blocks)),
        pinned_index_bytes_(pinned_index_bytes) {}

  const BlockBasedTable* table_;
  const uint32_t index_levels_;
  const BlockHandle pinned_root_handle_;
  CachableEntry<Block_kIndex> pinned_root_block_;
  const std::unordered_map<KVSepPinnedHandleKey, CachableEntry<Block_kIndex>,
                           KVSepPinnedHandleKeyHash>
      pinned_index_blocks_;
  const uint64_t pinned_index_bytes_;
};

Block* KVSepBptreeGetPinnedIndexBlock(const KVSepBptreeIndexReader* reader,
                                      const BlockHandle& handle) {
  if (reader == nullptr) {
    return nullptr;
  }
  return reader->GetPinnedIndexBlock(handle);
}

// Explicitly instantiate templates for each "blocklike" type we use (and
// before implicit specialization).
// This makes it possible to keep the template definitions in the .cc file.
#define INSTANTIATE_BLOCKLIKE_TEMPLATES(T)                                     \
  template Status BlockBasedTable::RetrieveBlock<T>(                           \
      FilePrefetchBuffer * prefetch_buffer, const ReadOptions& ro,             \
      const BlockHandle& handle, UnownedPtr<Decompressor> decomp,              \
      CachableEntry<T>* out_parsed_block, GetContext* get_context,             \
      BlockCacheLookupContext* lookup_context, bool for_compaction,            \
      bool use_cache, bool async_read, bool use_block_cache_for_lookup) const; \
  template Status BlockBasedTable::MaybeReadBlockAndLoadToCache<T>(            \
      FilePrefetchBuffer * prefetch_buffer, const ReadOptions& ro,             \
      const BlockHandle& handle, UnownedPtr<Decompressor> decomp,              \
      bool for_compaction, CachableEntry<T>* block_entry,                      \
      GetContext* get_context, BlockCacheLookupContext* lookup_context,        \
      BlockContents* contents, bool async_read,                                \
      bool use_block_cache_for_lookup) const;                                  \
  template Status BlockBasedTable::LookupAndPinBlocksInCache<T>(               \
      const ReadOptions& ro, const BlockHandle& handle,                        \
      CachableEntry<T>* out_parsed_block) const;                               \
  template Status BlockBasedTable::CreateAndPinBlockInCache<T>(                \
      const ReadOptions& ro, const BlockHandle& handle,                        \
      UnownedPtr<Decompressor> decomp, BlockContents* block_contents,          \
      CachableEntry<T>* out_parsed_block) const;

INSTANTIATE_BLOCKLIKE_TEMPLATES(ParsedFullFilterBlock);
INSTANTIATE_BLOCKLIKE_TEMPLATES(DecompressorDict);
INSTANTIATE_BLOCKLIKE_TEMPLATES(Block_kData);
INSTANTIATE_BLOCKLIKE_TEMPLATES(Block_kIndex);
INSTANTIATE_BLOCKLIKE_TEMPLATES(Block_kKVSepLeaf);
INSTANTIATE_BLOCKLIKE_TEMPLATES(Block_kKVSepPair);
INSTANTIATE_BLOCKLIKE_TEMPLATES(Block_kFilterPartitionIndex);
INSTANTIATE_BLOCKLIKE_TEMPLATES(Block_kRangeDeletion);
INSTANTIATE_BLOCKLIKE_TEMPLATES(Block_kMetaIndex);
INSTANTIATE_BLOCKLIKE_TEMPLATES(Block_kUserDefinedIndex);

}  // namespace ROCKSDB_NAMESPACE

// Generate the regular and coroutine versions of some methods by
// including block_based_table_reader_sync_and_async.h twice
// Macros in the header will expand differently based on whether
// WITH_COROUTINES or WITHOUT_COROUTINES is defined
// clang-format off
#define WITHOUT_COROUTINES
#include "table/block_based/block_based_table_reader_sync_and_async.h"
#undef WITHOUT_COROUTINES
#define WITH_COROUTINES
#include "table/block_based/block_based_table_reader_sync_and_async.h"
#undef WITH_COROUTINES
// clang-format on

namespace ROCKSDB_NAMESPACE {

extern const uint64_t kBlockBasedTableMagicNumber;
extern const std::string kHashIndexPrefixesBlock;
extern const std::string kHashIndexPrefixesMetadataBlock;

class KVSepBptreeLeafV2TableIterator final : public InternalIteratorBase<Slice> {
 public:
  KVSepBptreeLeafV2TableIterator(
      const BlockBasedTable* table, const ReadOptions& read_options,
      const InternalKeyComparator& icomp,
      std::unique_ptr<InternalIteratorBase<IndexValue>>&& index_iter,
      TableReaderCaller caller)
      : table_(table),
        read_options_(read_options),
        icomp_(icomp),
        index_iter_(std::move(index_iter)),
        lookup_context_(caller),
        prefetcher_(/*compaction_readahead_size=*/0,
                    table->get_rep()->table_options.initial_auto_readahead_size) {}

  bool Valid() const override {
    return status_.ok() && index_iter_ && index_iter_->Valid() && leaf_loaded_ &&
           leaf_idx_ < leaf_view_.num_entries();
  }

  void SeekToFirst() override {
    ResetState();
    if (!index_iter_) {
      status_ = Status::InvalidArgument("kvsep leaf v2: missing index iterator");
      return;
    }
    index_iter_->SeekToFirst();
    if (!index_iter_->Valid()) {
      return;
    }
    if (!LoadLeaf(index_iter_->value().handle)) {
      return;
    }
    leaf_idx_ = 0;
    UpdateKeyScratch();
  }

  void SeekToLast() override {
    ResetState();
    if (!index_iter_) {
      status_ = Status::InvalidArgument("kvsep leaf v2: missing index iterator");
      return;
    }
    index_iter_->SeekToLast();
    if (!index_iter_->Valid()) {
      return;
    }
    if (!LoadLeaf(index_iter_->value().handle)) {
      return;
    }
    if (leaf_view_.num_entries() == 0) {
      status_ = Status::Corruption("kvsep leaf v2: empty leaf block");
      return;
    }
    leaf_idx_ = leaf_view_.num_entries() - 1;
    UpdateKeyScratch();
  }

  void Seek(const Slice& target) override {
    ResetState();
    if (!index_iter_) {
      status_ = Status::InvalidArgument("kvsep leaf v2: missing index iterator");
      return;
    }

    index_iter_->Seek(target);
    while (index_iter_->Valid()) {
      if (!LoadLeaf(index_iter_->value().handle)) {
        return;
      }
      uint32_t pos = LowerBoundInLeaf(target);
      if (pos < leaf_view_.num_entries()) {
        leaf_idx_ = pos;
        UpdateKeyScratch();
        return;
      }
      index_iter_->Next();
    }
  }

  void SeekForPrev(const Slice& target) override {
    Seek(target);
    if (!Valid()) {
      SeekToLast();
    }
    while (Valid() && icomp_.Compare(key(), target) > 0) {
      Prev();
    }
  }

  void Next() override {
    if (!Valid()) {
      return;
    }
    ++leaf_idx_;
    if (leaf_idx_ < leaf_view_.num_entries()) {
      UpdateKeyScratch();
      return;
    }
    index_iter_->Next();
    while (index_iter_->Valid()) {
      if (!LoadLeaf(index_iter_->value().handle)) {
        return;
      }
      if (leaf_view_.num_entries() > 0) {
        leaf_idx_ = 0;
        UpdateKeyScratch();
        return;
      }
      index_iter_->Next();
    }
  }

  void Prev() override {
    if (!Valid()) {
      return;
    }
    if (leaf_idx_ > 0) {
      --leaf_idx_;
      UpdateKeyScratch();
      return;
    }
    index_iter_->Prev();
    while (index_iter_->Valid()) {
      if (!LoadLeaf(index_iter_->value().handle)) {
        return;
      }
      if (leaf_view_.num_entries() > 0) {
        leaf_idx_ = leaf_view_.num_entries() - 1;
        UpdateKeyScratch();
        return;
      }
      index_iter_->Prev();
    }
  }

  Slice key() const override {
    assert(Valid());
    return Slice(key_scratch_);
  }

  Slice user_key() const override {
    assert(Valid());
    return ExtractUserKey(key());
  }

  Slice value() const override {
    assert(Valid());
    if (!status_.ok()) {
      return Slice();
    }
    const uint32_t value_len = leaf_view_.ValueLenAt(leaf_idx_);
    if (value_len == 0) {
      return Slice();
    }
    const BlockHandle& vb_handle = leaf_view_.value_block_handle();
    if (vb_handle.IsNull()) {
      const_cast<KVSepBptreeLeafV2TableIterator*>(this)->status_ =
          Status::Corruption("kvsep leaf v2: missing value block handle");
      return Slice();
    }

    const bool need_reload =
        kvsep_value_block_.GetValue() == nullptr ||
        kvsep_value_block_handle_.offset() != vb_handle.offset() ||
        kvsep_value_block_handle_.size() != vb_handle.size();
    if (need_reload) {
      kvsep_value_block_.Reset();
      kvsep_value_block_handle_ = vb_handle;
      auto* self = const_cast<KVSepBptreeLeafV2TableIterator*>(this);
      self->status_ = table_->KVSepBptreeGetValueBlock(
          read_options_, vb_handle, &kvsep_value_block_, &lookup_context_,
          PrefetchBuffer());
      if (!self->status_.ok()) {
        return Slice();
      }
    }

    if (UNLIKELY(kvsep_value_block_.GetValue() == nullptr)) {
      const_cast<KVSepBptreeLeafV2TableIterator*>(this)->status_ =
          Status::Corruption("kvsep leaf v2: missing value block");
      return Slice();
    }
    const Slice value_block_contents =
        kvsep_value_block_.GetValue()->ContentSlice();
    const uint32_t value_off = leaf_view_.ValueOffAt(leaf_idx_);
    if (UNLIKELY(static_cast<size_t>(value_off) + static_cast<size_t>(value_len) >
                 value_block_contents.size())) {
      const_cast<KVSepBptreeLeafV2TableIterator*>(this)->status_ =
          Status::Corruption("kvsep leaf v2: value pointer out of range");
      return Slice();
    }
    return Slice(value_block_contents.data() + value_off, value_len);
  }

  Status status() const override {
    if (!status_.ok()) {
      return status_;
    }
    if (index_iter_ && !index_iter_->status().ok() &&
        !index_iter_->status().IsNotFound()) {
      return index_iter_->status();
    }
    return Status::OK();
  }

  bool PrepareValue() override { return status().ok(); }
  void SetPinnedItersMgr(PinnedIteratorsManager*) override {}
  bool IsKeyPinned() const override { return false; }
  bool IsValuePinned() const override { return false; }

 private:
  const BlockBasedTable* table_;
  const ReadOptions read_options_;
  const InternalKeyComparator& icomp_;
  std::unique_ptr<InternalIteratorBase<IndexValue>> index_iter_;
  mutable BlockCacheLookupContext lookup_context_;
  mutable std::unique_ptr<FilePrefetchBuffer> prefetch_buffer_;

  Status status_ = Status::OK();

  // Current leaf state.
  bool leaf_loaded_ = false;
  BlockHandle leaf_handle_ = BlockHandle::NullBlockHandle();
  CachableEntry<Block_kKVSepLeaf> leaf_block_;
  KVSepBptreeLeafV2View leaf_view_;
  uint32_t leaf_idx_ = 0;
  mutable std::string key_scratch_;
  mutable std::string key_compare_scratch_;

  // Value-only blocks.
  mutable CachableEntry<Block_kKVSepValue> kvsep_value_block_;
  mutable BlockHandle kvsep_value_block_handle_ = BlockHandle::NullBlockHandle();
  mutable BlockPrefetcher prefetcher_;

  FilePrefetchBuffer* PrefetchBuffer() const {
    CreateExperimentalKVSepPrefetchBufferIfNeeded(
        table_->get_rep(), read_options_, &prefetch_buffer_,
        /*fallback_readahead_bytes=*/64 * 1024,
        FilePrefetchBufferUsage::kUnknown);
    return prefetch_buffer_.get();
  }

  void ResetState() {
    status_ = Status::OK();
    leaf_loaded_ = false;
    leaf_handle_ = BlockHandle::NullBlockHandle();
    leaf_block_.Reset();
    leaf_idx_ = 0;
    key_scratch_.clear();
    key_compare_scratch_.clear();
    kvsep_value_block_.Reset();
    kvsep_value_block_handle_ = BlockHandle::NullBlockHandle();
  }

  void UpdateKeyScratch() const {
    key_scratch_.clear();
    key_scratch_.append(leaf_view_.prefix().data(), leaf_view_.prefix().size());
    const Slice suffix = leaf_view_.SuffixAt(leaf_idx_);
    key_scratch_.append(suffix.data(), suffix.size());
  }

  bool LoadLeaf(const BlockHandle& handle) {
    if (leaf_loaded_ && handle.offset() == leaf_handle_.offset() &&
        handle.size() == leaf_handle_.size()) {
      return true;
    }
    leaf_loaded_ = false;
    leaf_handle_ = handle;
    leaf_block_.Reset();

    static const std::function<void(bool, uint64_t&, uint64_t&)> kNoopReadaheadCb =
        [](bool, uint64_t&, uint64_t&) {};
    prefetcher_.PrefetchIfNeeded(
        table_->get_rep(), handle, read_options_.readahead_size,
        /*is_for_compaction=*/false,
        /*no_sequential_checking=*/false, read_options_, kNoopReadaheadCb,
        /*is_async_io_prefetch=*/false);

    FilePrefetchBuffer* const fpb =
        prefetcher_.prefetch_buffer() ? prefetcher_.prefetch_buffer()
                                      : PrefetchBuffer();
    Status s;
    s = table_->RetrieveBlock<Block_kKVSepLeaf>(
        /*prefetch_buffer=*/fpb, read_options_, handle,
        table_->get_rep()->decompressor.get(), &leaf_block_,
        /*get_context=*/nullptr, &lookup_context_, /*for_compaction=*/false,
        /*use_cache=*/read_options_.fill_cache, /*async_read=*/false,
        /*use_block_cache_for_lookup=*/true);
    if (!s.ok()) {
      status_ = s;
      return false;
    }
    if (fpb) {
      fpb->UpdateReadPattern(handle.offset(),
                             BlockBasedTable::BlockSizeWithTrailer(handle),
                             read_options_.adaptive_readahead);
    }
    if (leaf_block_.GetValue() == nullptr) {
      status_ = Status::Corruption("kvsep leaf v2: missing leaf block");
      return false;
    }
    Status parse_s =
        leaf_view_.InitFromContents(leaf_block_.GetValue()->ContentSlice());
    if (!parse_s.ok()) {
      status_ = parse_s;
      return false;
    }
    leaf_loaded_ = true;
    // Value blocks are per-leaf; invalidate cached value block when leaf moves.
    kvsep_value_block_.Reset();
    kvsep_value_block_handle_ = BlockHandle::NullBlockHandle();
    return true;
  }

  uint32_t LowerBoundInLeaf(const Slice& target) const {
    uint32_t left = 0;
    uint32_t right = leaf_view_.num_entries();
    const bool bytewise =
        KVSepBptreeIsBytewiseComparator(icomp_.user_comparator());
    const size_t prefix_len = leaf_view_.prefix().size();
    if (prefix_len > 0) {
      // Prepare scratch with the prefix once so we can avoid clearing and
      // re-appending it for every comparison.
      key_compare_scratch_.assign(leaf_view_.prefix().data(), prefix_len);
    }
    while (left < right) {
      const uint32_t mid = left + (right - left) / 2;
      const Slice mid_suffix = leaf_view_.SuffixAt(mid);
      int cmp = 0;
      if (prefix_len == 0) {
        cmp = icomp_.Compare(mid_suffix, target);
      } else if (bytewise) {
        // Avoid materializing the full key when we can compare as
        // {prefix,suffix} directly under bytewise comparator semantics.
        cmp = KVSepBptreeCompareInternalKeyBytewise(leaf_view_.prefix(),
                                                    mid_suffix, target);
      } else {
        // Fallback: materialize a contiguous key to respect arbitrary user
        // comparator semantics.
        const Slice mid_key = leaf_view_.FullKeyAtWithScratchPrefix(
            mid, &key_compare_scratch_, prefix_len);
        cmp = icomp_.Compare(mid_key, target);
      }
      if (cmp < 0) {
        left = mid + 1;
      } else {
        right = mid;
      }
    }
    return left;
  }
};

class KVSepBptreePairV3TableIterator final : public InternalIteratorBase<Slice> {
 public:
  enum class PairLoadReason : uint8_t { kSeek, kScan };

  KVSepBptreePairV3TableIterator(
      const BlockBasedTable* table, const ReadOptions& read_options,
      const InternalKeyComparator& icomp,
      std::unique_ptr<InternalIteratorBase<IndexValue>>&& index_iter,
      TableReaderCaller caller)
      : table_(table),
        read_options_(read_options),
        icomp_(icomp),
        index_iter_(std::move(index_iter)),
        lookup_context_(caller),
        prefetcher_(/*compaction_readahead_size=*/0,
                    table->get_rep()->table_options.initial_auto_readahead_size) {}

  bool Valid() const override {
    if (!status_.ok() || !index_iter_ || !index_iter_->Valid()) {
      return false;
    }
    if (at_first_key_from_index_) {
      return true;
    }
    return pair_loaded_ && leaf_idx_ < leaf_view_.num_entries();
  }

  void SeekToFirst() override {
    ResetState();
    if (!index_iter_) {
      status_ = Status::InvalidArgument("kvsep pair v3: missing index iterator");
      return;
    }
    index_iter_->SeekToFirst();
    if (!index_iter_->Valid()) {
      return;
    }
    if (!LoadPair(index_iter_->value().handle, /*prefetch_buffer=*/ScanPrefetchBuffer(),
                  PairLoadReason::kScan)) {
      return;
    }
    leaf_idx_ = 0;
    UpdateKeyScratch();
  }

  void SeekToLast() override {
    ResetState();
    if (!index_iter_) {
      status_ = Status::InvalidArgument("kvsep pair v3: missing index iterator");
      return;
    }
    index_iter_->SeekToLast();
    if (!index_iter_->Valid()) {
      return;
    }
    if (!LoadPair(index_iter_->value().handle, /*prefetch_buffer=*/ScanPrefetchBuffer(),
                  PairLoadReason::kScan)) {
      return;
    }
    if (leaf_view_.num_entries() == 0) {
      status_ = Status::Corruption("kvsep pair v3: empty leaf payload");
      return;
    }
    leaf_idx_ = leaf_view_.num_entries() - 1;
    UpdateKeyScratch();
  }

  void Seek(const Slice& target) override {
    ResetSeekState();
    if (!index_iter_) {
      status_ = Status::InvalidArgument("kvsep pair v3: missing index iterator");
      return;
    }

    index_iter_->Seek(target);
    while (index_iter_->Valid()) {
      const IndexValue v = index_iter_->value();
      // Support the "index with first key" optimization: if the index entry
      // includes the first key of the block, and it's already >= target, we
      // can defer loading the pair block until the iterator is actually used.
      if (!v.first_internal_key.empty() &&
          icomp_.Compare(target, v.first_internal_key) <= 0) {
        at_first_key_from_index_ = true;
        first_key_from_index_.assign(v.first_internal_key.data(),
                                     v.first_internal_key.size());
        return;
      }
      // Seek-heavy workloads (mixgraph) tend to be random at the block level,
      // where FilePrefetchBuffer rarely helps but can interfere with super-block
      // aligned read coalescing. Prefer the super-block cache path for seeks.
      if (!LoadPair(index_iter_->value().handle, /*prefetch_buffer=*/nullptr,
                    PairLoadReason::kSeek)) {
        return;
      }
      uint32_t pos = LowerBoundInLeaf(target);
      if (pos < leaf_view_.num_entries()) {
        leaf_idx_ = pos;
        UpdateKeyScratch();
        return;
      }
      index_iter_->Next();
    }
  }

  void SeekForPrev(const Slice& target) override {
    Seek(target);
    if (!Valid()) {
      SeekToLast();
    }
    while (Valid() && icomp_.Compare(key(), target) > 0) {
      Prev();
    }
  }

  void Next() override {
    if (at_first_key_from_index_) {
      // Materialize the deferred first key so we can advance within the block.
      if (!index_iter_ || !index_iter_->Valid()) {
        return;
      }
      if (!LoadPair(index_iter_->value().handle,
                    /*prefetch_buffer=*/ScanPrefetchBuffer(),
                    PairLoadReason::kScan)) {
        return;
      }
      leaf_idx_ = 0;
      UpdateKeyScratch();
      at_first_key_from_index_ = false;
    }
    if (!Valid()) {
      return;
    }
    ++leaf_idx_;
    if (leaf_idx_ < leaf_view_.num_entries()) {
      UpdateKeyScratch();
      return;
    }
    index_iter_->Next();
    while (index_iter_->Valid()) {
      if (!LoadPair(index_iter_->value().handle, /*prefetch_buffer=*/ScanPrefetchBuffer(),
                    PairLoadReason::kScan)) {
        return;
      }
      if (leaf_view_.num_entries() > 0) {
        leaf_idx_ = 0;
        UpdateKeyScratch();
        return;
      }
      index_iter_->Next();
    }
  }

  void Prev() override {
    if (at_first_key_from_index_) {
      // Materialize the deferred first key so we can move backwards.
      if (!index_iter_ || !index_iter_->Valid()) {
        return;
      }
      if (!LoadPair(index_iter_->value().handle,
                    /*prefetch_buffer=*/ScanPrefetchBuffer(),
                    PairLoadReason::kScan)) {
        return;
      }
      leaf_idx_ = 0;
      UpdateKeyScratch();
      at_first_key_from_index_ = false;
    }
    if (!Valid()) {
      return;
    }
    if (leaf_idx_ > 0) {
      --leaf_idx_;
      UpdateKeyScratch();
      return;
    }
    index_iter_->Prev();
    while (index_iter_->Valid()) {
      if (!LoadPair(index_iter_->value().handle, /*prefetch_buffer=*/ScanPrefetchBuffer(),
                    PairLoadReason::kScan)) {
        return;
      }
      if (leaf_view_.num_entries() > 0) {
        leaf_idx_ = leaf_view_.num_entries() - 1;
        UpdateKeyScratch();
        return;
      }
      index_iter_->Prev();
    }
  }

  Slice key() const override {
    assert(Valid());
    if (at_first_key_from_index_) {
      return Slice(first_key_from_index_);
    }
    if (scratch_prefix_len_ == 0) {
      return leaf_view_.SuffixAt(leaf_idx_);
    }
    return Slice(key_scratch_);
  }

  Slice user_key() const override {
    assert(Valid());
    return ExtractUserKey(key());
  }

  Slice value() const override {
    assert(Valid());
    if (!status_.ok()) {
      return Slice();
    }
    if (at_first_key_from_index_) {
      // Defer block load until value access.
      auto* self = const_cast<KVSepBptreePairV3TableIterator*>(this);
      if (!self->LoadPair(index_iter_->value().handle,
                          /*prefetch_buffer=*/self->SeekPrefetchBuffer(),
                          PairLoadReason::kSeek)) {
        return Slice();
      }
      self->leaf_idx_ = 0;
      self->UpdateKeyScratch();
      self->at_first_key_from_index_ = false;
    }
    const uint32_t value_len = leaf_view_.ValueLenAt(leaf_idx_);
    if (value_len == 0) {
      return Slice();
    }
    if (UNLIKELY(pair_block_.GetValue() == nullptr)) {
      const_cast<KVSepBptreePairV3TableIterator*>(this)->status_ =
          Status::Corruption("kvsep pair v3: missing pair block");
      return Slice();
    }

    const Slice value_block_contents = pair_view_.value_contents();
    const uint32_t value_off = leaf_view_.ValueOffAt(leaf_idx_);
    if (UNLIKELY(static_cast<size_t>(value_off) + static_cast<size_t>(value_len) >
                 value_block_contents.size())) {
      const_cast<KVSepBptreePairV3TableIterator*>(this)->status_ =
          Status::Corruption("kvsep pair v3: value pointer out of range");
      return Slice();
    }
    return Slice(value_block_contents.data() + value_off, value_len);
  }

  Status status() const override {
    if (!status_.ok()) {
      return status_;
    }
    if (index_iter_ && !index_iter_->status().ok() &&
        !index_iter_->status().IsNotFound()) {
      return index_iter_->status();
    }
    return Status::OK();
  }

  bool PrepareValue() override { return status().ok(); }
  void SetPinnedItersMgr(PinnedIteratorsManager*) override {}
  bool IsKeyPinned() const override { return false; }
  bool IsValuePinned() const override { return false; }

 private:
  const BlockBasedTable* table_;
  const ReadOptions read_options_;
  const InternalKeyComparator& icomp_;
  std::unique_ptr<InternalIteratorBase<IndexValue>> index_iter_;
  mutable BlockCacheLookupContext lookup_context_;
  mutable std::unique_ptr<FilePrefetchBuffer> seek_prefetch_buffer_;
  mutable std::unique_ptr<FilePrefetchBuffer> scan_prefetch_buffer_;
  mutable BlockPrefetcher prefetcher_;
  // Local scan prefetch tracking for simfs:
  // The built-in BlockBasedTableIterator issues file->Prefetch() calls on
  // sequential scans when auto readahead is enabled, which the simfs model
  // can treat as "prefetch hits" on subsequent reads. The pair-v3 iterator
  // bypasses that iterator, so replicate the minimal behavior here for scan
  // reads only.
  mutable uint64_t scan_prefetch_limit_ = 0;
  mutable size_t scan_readahead_size_ = 0;
  mutable uint64_t scan_num_file_reads_ = 0;
  mutable uint64_t scan_prev_offset_ = 0;
  mutable size_t scan_prev_len_ = 0;

  Status status_ = Status::OK();
  bool at_first_key_from_index_ = false;
  std::string first_key_from_index_;

  // Current pair/leaf state.
  bool pair_loaded_ = false;
  BlockHandle pair_handle_ = BlockHandle::NullBlockHandle();
  CachableEntry<Block_kKVSepPair> pair_block_;
  KVSepBptreePairV3View pair_view_;
  KVSepBptreeLeafV3View leaf_view_;
  uint32_t leaf_idx_ = 0;
  mutable std::string key_scratch_;
  mutable std::string key_compare_scratch_;
  mutable size_t scratch_prefix_len_ = 0;

  FilePrefetchBuffer* SeekPrefetchBuffer() const {
    CreateExperimentalKVSepPrefetchBufferIfNeeded(
        table_->get_rep(), read_options_, &seek_prefetch_buffer_,
        /*fallback_readahead_bytes=*/0, FilePrefetchBufferUsage::kUnknown,
        /*allow_implicit_readahead=*/false);
    return seek_prefetch_buffer_.get();
  }

  FilePrefetchBuffer* ScanPrefetchBuffer() const {
    size_t fallback = table_->get_rep()->table_options.super_block_alignment_size;
    if (fallback == 0) {
      fallback = 16 * 1024;
    }
    CreateExperimentalKVSepPrefetchBufferIfNeeded(
        table_->get_rep(), read_options_, &scan_prefetch_buffer_,
        // Provide a small implicit fallback for sequential Next() scans.
        // Use the configured super-block alignment size as the default, so we
        // avoid over-fetching on short scans.
        /*fallback_readahead_bytes=*/fallback,
        FilePrefetchBufferUsage::kUserScanPrefetch,
        /*allow_implicit_readahead=*/true);
    return scan_prefetch_buffer_.get();
  }

  void ResetState() {
    status_ = Status::OK();
    at_first_key_from_index_ = false;
    first_key_from_index_.clear();
    pair_loaded_ = false;
    pair_handle_ = BlockHandle::NullBlockHandle();
    pair_block_.Reset();
    leaf_idx_ = 0;
    key_scratch_.clear();
    key_compare_scratch_.clear();
    scratch_prefix_len_ = 0;
  }

  void ResetSeekState() {
    status_ = Status::OK();
    at_first_key_from_index_ = false;
    first_key_from_index_.clear();
    leaf_idx_ = 0;
    // Intentionally keep {pair_loaded_, pair_handle_, pair_block_} so repeated
    // seeks within the same pair/leaf can reuse the already-loaded block.
  }

  void UpdateKeyScratch() const {
    if (scratch_prefix_len_ == 0) {
      return;
    }
    key_scratch_.resize(scratch_prefix_len_);
    const Slice suffix = leaf_view_.SuffixAt(leaf_idx_);
    key_scratch_.resize(scratch_prefix_len_ + suffix.size());
    if (!suffix.empty()) {
      memcpy(&key_scratch_[scratch_prefix_len_], suffix.data(), suffix.size());
    }
  }

  bool LoadPair(const BlockHandle& handle, FilePrefetchBuffer* prefetch_buffer,
                PairLoadReason reason) {
    if (pair_loaded_ && handle.offset() == pair_handle_.offset() &&
        handle.size() == pair_handle_.size()) {
      return true;
    }
    pair_loaded_ = false;
    pair_handle_ = handle;
    pair_block_.Reset();

    if (reason == PairLoadReason::kScan) {
      MaybeSimFsPrefetchForScan(handle);
    }

    FilePrefetchBuffer* fpb = prefetch_buffer;
    static const std::function<void(bool, uint64_t&, uint64_t&)> kNoopReadaheadCb =
        [](bool, uint64_t&, uint64_t&) {};
    if (reason == PairLoadReason::kScan) {
      prefetcher_.PrefetchIfNeeded(
          table_->get_rep(), handle, read_options_.readahead_size,
          /*is_for_compaction=*/false,
          /*no_sequential_checking=*/false, read_options_, kNoopReadaheadCb,
          /*is_async_io_prefetch=*/false);
      if (prefetcher_.prefetch_buffer() != nullptr) {
        fpb = prefetcher_.prefetch_buffer();
      }
    }

    Status s;
    s = table_->RetrieveBlock<Block_kKVSepPair>(
        /*prefetch_buffer=*/fpb, read_options_, handle,
        table_->get_rep()->decompressor.get(), &pair_block_,
        /*get_context=*/nullptr, &lookup_context_, /*for_compaction=*/false,
        /*use_cache=*/read_options_.fill_cache, /*async_read=*/false,
        /*use_block_cache_for_lookup=*/true);
    if (!s.ok()) {
      status_ = s;
      return false;
    }
    if (!pair_block_.IsCached()) {
      const uint64_t bytes = BlockBasedTable::BlockSizeWithTrailer(handle);
      auto* stats = table_->GetStatistics();
      if (reason == PairLoadReason::kSeek) {
        RecordTick(stats, EXPERIMENTAL_KVSEP_BPTREE_PAIR_FILE_READS_SEEK, 1);
        RecordTick(stats, EXPERIMENTAL_KVSEP_BPTREE_PAIR_FILE_READ_BYTES_SEEK,
                   bytes);
      } else {
        RecordTick(stats, EXPERIMENTAL_KVSEP_BPTREE_PAIR_FILE_READS_SCAN, 1);
        RecordTick(stats, EXPERIMENTAL_KVSEP_BPTREE_PAIR_FILE_READ_BYTES_SCAN,
                   bytes);
      }
    }
    if (fpb) {
      fpb->UpdateReadPattern(handle.offset(),
                                         BlockBasedTable::BlockSizeWithTrailer(handle),
                                         read_options_.adaptive_readahead);
    }
    if (pair_block_.GetValue() == nullptr) {
      status_ = Status::Corruption("kvsep pair v3: missing pair block");
      return false;
    }
    Status parse_pair =
        pair_view_.InitFromContents(pair_block_.GetValue()->ContentSlice());
    if (!parse_pair.ok()) {
      status_ = parse_pair;
      return false;
    }
    Status parse_leaf = leaf_view_.InitFromContents(pair_view_.leaf_contents());
    if (!parse_leaf.ok()) {
      status_ = parse_leaf;
      return false;
    }
    scratch_prefix_len_ = leaf_view_.prefix().size();
    // Ensure scratch buffers always contain the prefix to avoid re-appending it
    // on every key comparison/iteration.
    key_scratch_.assign(leaf_view_.prefix().data(), leaf_view_.prefix().size());
    key_compare_scratch_.assign(leaf_view_.prefix().data(),
                                leaf_view_.prefix().size());
    pair_loaded_ = true;
    return true;
  }

  void MaybeSimFsPrefetchForScan(const BlockHandle& handle) const {
    if (read_options_.read_tier == ReadTier::kBlockCacheTier) {
      return;
    }
    const auto* rep = table_->get_rep();
    if (rep == nullptr || rep->file == nullptr) {
      return;
    }

    const size_t initial = rep->table_options.initial_auto_readahead_size;
    const size_t max = rep->table_options.max_auto_readahead_size;
    if (initial == 0 || max == 0) {
      return;
    }
    if (scan_readahead_size_ == 0) {
      scan_readahead_size_ = initial;
    }

    // Important: simfs models prefetch hits at the file offset granularity. With
    // super-block aligned reads enabled, the actual IOs happen on aligned
    // ranges, not the raw block handle {offset,size}. If we use the raw handle
    // here, two consecutive on-disk blocks can look "non-sequential" (different
    // offsets after alignment rounding), preventing readahead ramp-up and
    // under-reporting prefetch effectiveness vs. the baseline iterator.
    uint64_t offset = handle.offset();
    size_t len = BlockBasedTable::BlockSizeWithTrailer(handle);
    const size_t align = rep->table_options.super_block_alignment_size;
    if (align > 0 && rep->table_options.enable_super_block_read_coalescing) {
      const uint64_t end = offset + static_cast<uint64_t>(len);
      const uint64_t aligned_off = (offset / align) * align;
      const uint64_t aligned_end = ((end + align - 1) / align) * align;
      offset = aligned_off;
      len = static_cast<size_t>(aligned_end - aligned_off);
    }

    const bool sequential =
        (scan_prev_len_ == 0) || (scan_prev_offset_ + scan_prev_len_ == offset);
    scan_prev_offset_ = offset;
    scan_prev_len_ = len;

    if (!sequential) {
      scan_num_file_reads_ = 1;
      scan_readahead_size_ = initial;
      scan_prefetch_limit_ = 0;
      return;
    }

    // Mirror BlockPrefetcher behavior: enable after N sequential IOs.
    scan_num_file_reads_++;
    if (scan_num_file_reads_ <= rep->table_options.num_file_reads_for_auto_readahead) {
      return;
    }

    if (offset + len <= scan_prefetch_limit_) {
      return;
    }

    IOOptions opts;
    IODebugContext dbg;
    Status s = rep->file->PrepareIOOptions(read_options_, opts, &dbg);
    if (!s.ok()) {
      return;
    }
    // Best-effort: even if the underlying FS does not support prefetch, simfs
    // still accounts it and may treat following reads as prefetch hits.
    (void)rep->file->Prefetch(opts, offset, len + scan_readahead_size_, &dbg);
    scan_prefetch_limit_ = offset + len + scan_readahead_size_;
    scan_readahead_size_ = std::min(max, scan_readahead_size_ * 2);
  }

  uint32_t LowerBoundInLeaf(const Slice& target) const {
    uint32_t left = 0;
    uint32_t right = leaf_view_.num_entries();
    const bool bytewise =
        KVSepBptreeIsBytewiseComparator(icomp_.user_comparator());
    while (left < right) {
      const uint32_t mid = left + (right - left) / 2;
      const Slice suffix = leaf_view_.SuffixAt(mid);
      int cmp = 0;
      if (scratch_prefix_len_ == 0) {
        cmp = icomp_.Compare(suffix, target);
      } else if (bytewise) {
        cmp = KVSepBptreeCompareInternalKeyBytewise(leaf_view_.prefix(), suffix,
                                                    target);
      } else {
        key_compare_scratch_.resize(scratch_prefix_len_ + suffix.size());
        if (!suffix.empty()) {
          memcpy(&key_compare_scratch_[scratch_prefix_len_], suffix.data(),
                 suffix.size());
        }
        cmp = icomp_.Compare(Slice(key_compare_scratch_), target);
      }
      if (cmp < 0) {
        left = mid + 1;
      } else {
        right = mid;
      }
    }
    return left;
  }
};

BlockBasedTable::~BlockBasedTable() {
  auto ua = rep_->uncache_aggressiveness.LoadRelaxed();
  // NOTE: there is an undiagnosed incompatibility with mmap reads,
  // where attempting to read the index below can result in bus error.
  // In theory the mmap should remain in place until destruction of
  // rep_, so even a page fault should be satisfiable. But also, combining
  // mmap reads with block cache is weird, so it's not a concerning loss.
  if (ua > 0 && rep_->table_options.block_cache &&
      !rep_->ioptions.allow_mmap_reads) {
    if (rep_->filter) {
      rep_->filter->EraseFromCacheBeforeDestruction(ua);
    }
    if (rep_->index_reader) {
      {
        // TODO: Also uncache data blocks known after any gaps in partitioned
        // index. Right now the iterator errors out as soon as there's an
        // index partition not in cache.
        IndexBlockIter iiter_on_stack;
        ReadOptions ropts;
        ropts.read_tier = kBlockCacheTier;  // No I/O
        auto iiter = NewIndexIterator(
            ropts, /*disable_prefix_seek=*/false, &iiter_on_stack,
            /*get_context=*/nullptr, /*lookup_context=*/nullptr);
        std::unique_ptr<InternalIteratorBase<IndexValue>> iiter_unique_ptr;
        if (iiter != &iiter_on_stack) {
          iiter_unique_ptr.reset(iiter);
        }
        // Un-cache the data blocks the index iterator with tell us about
        // without I/O. (NOTE: It's extremely unlikely that a data block
        // will be in block cache without the index block pointing to it
        // also in block cache.)
        UncacheAggressivenessAdvisor advisor(ua);
        for (iiter->SeekToFirst(); iiter->Valid() && advisor.ShouldContinue();
             iiter->Next()) {
          bool erased = EraseFromCache(iiter->value().handle);
          advisor.Report(erased);
        }
        iiter->status().PermitUncheckedError();
      }

      // Un-cache the index block(s)
      rep_->index_reader->EraseFromCacheBeforeDestruction(ua);
    }
  }
  delete rep_;
}

namespace {
// Read the block identified by "handle" from "file".
// The only relevant option is options.verify_checksums for now.
// On failure return non-OK.
// On success fill *result and return OK - caller owns *result
// @param uncompression_dict Data for presetting the compression library's
//    dictionary.
template <typename TBlocklike>
Status ReadAndParseBlockFromFile(
    RandomAccessFileReader* file, FilePrefetchBuffer* prefetch_buffer,
    const Footer& footer, const ReadOptions& options, const BlockHandle& handle,
    std::unique_ptr<TBlocklike>* result, const ImmutableOptions& ioptions,
    BlockCreateContext& create_context, bool maybe_compressed,
    UnownedPtr<Decompressor> decomp,
    const PersistentCacheOptions& cache_options,
    MemoryAllocator* memory_allocator, bool for_compaction, bool async_read) {
  assert(result);

  // Use the table options (if available) for super block alignment / coalescing.
  // This is important for experimental paths that bypass block cache, so they
  // still benefit from coalesced reads.
  const uint64_t super_block_alignment_size =
      create_context.table_options
          ? create_context.table_options->super_block_alignment_size
          : 0;
  const bool enable_super_block_read_coalescing =
      create_context.table_options
          ? create_context.table_options->enable_super_block_read_coalescing
          : false;

  BlockContents contents;
  BlockFetcher block_fetcher(
      file, prefetch_buffer, footer, options, handle, &contents,
      super_block_alignment_size, enable_super_block_read_coalescing, ioptions,
      /*do_uncompress*/ maybe_compressed, maybe_compressed,
      TBlocklike::kBlockType, decomp, cache_options, memory_allocator, nullptr,
      for_compaction);
  Status s;
  // If prefetch_buffer is not allocated, it will fallback to synchronous
  // reading of block contents.
  if (async_read && prefetch_buffer != nullptr) {
    s = block_fetcher.ReadAsyncBlockContents();
    if (!s.ok()) {
      return s;
    }
  } else {
    s = block_fetcher.ReadBlockContents();
  }
  if (s.ok()) {
    create_context.Create(result, std::move(contents));
  }
  return s;
}

// For hash based index, return false if table_properties->prefix_extractor_name
// and prefix_extractor both exist and match, otherwise true.
inline bool PrefixExtractorChangedHelper(
    const TableProperties* table_properties,
    const SliceTransform* prefix_extractor) {
  // BlockBasedTableOptions::kHashSearch requires prefix_extractor to be set.
  // Turn off hash index in prefix_extractor is not set; if  prefix_extractor
  // is set but prefix_extractor_block is not set, also disable hash index
  if (prefix_extractor == nullptr || table_properties == nullptr ||
      table_properties->prefix_extractor_name.empty()) {
    return true;
  }

  // prefix_extractor and prefix_extractor_block are both non-empty
  if (table_properties->prefix_extractor_name != prefix_extractor->AsString()) {
    return true;
  } else {
    return false;
  }
}

template <typename TBlocklike>
uint32_t GetBlockNumRestarts(const TBlocklike& block) {
  if constexpr (std::is_convertible_v<const TBlocklike&, const Block&>) {
    const Block& b = block;
    return b.NumRestarts();
  } else {
    return 0;
  }
}

}  // namespace

void BlockBasedTable::UpdateCacheHitMetrics(BlockType block_type,
                                            GetContext* get_context,
                                            size_t usage) const {
  Statistics* const statistics = rep_->ioptions.stats;

  PERF_COUNTER_ADD(block_cache_hit_count, 1);
  PERF_COUNTER_ADD(block_cache_read_byte, usage);
  PERF_COUNTER_BY_LEVEL_ADD(block_cache_hit_count, 1,
                            static_cast<uint32_t>(rep_->level));

  if (get_context) {
    ++get_context->get_context_stats_.num_cache_hit;
    get_context->get_context_stats_.num_cache_bytes_read += usage;
  } else {
    RecordTick(statistics, BLOCK_CACHE_HIT);
    RecordTick(statistics, BLOCK_CACHE_BYTES_READ, usage);
  }

  switch (block_type) {
    case BlockType::kFilter:
    case BlockType::kFilterPartitionIndex:
      PERF_COUNTER_ADD(block_cache_filter_hit_count, 1);
      PERF_COUNTER_ADD(block_cache_filter_read_byte, usage);

      if (get_context) {
        ++get_context->get_context_stats_.num_cache_filter_hit;
      } else {
        RecordTick(statistics, BLOCK_CACHE_FILTER_HIT);
      }
      break;

    case BlockType::kCompressionDictionary:
      // TODO: introduce perf counter for compression dictionary hit count
      PERF_COUNTER_ADD(block_cache_compression_dict_read_byte, usage);
      if (get_context) {
        ++get_context->get_context_stats_.num_cache_compression_dict_hit;
      } else {
        RecordTick(statistics, BLOCK_CACHE_COMPRESSION_DICT_HIT);
      }
      break;

    case BlockType::kIndex:
      PERF_COUNTER_ADD(block_cache_index_hit_count, 1);
      PERF_COUNTER_ADD(block_cache_index_read_byte, usage);

      if (get_context) {
        ++get_context->get_context_stats_.num_cache_index_hit;
      } else {
        RecordTick(statistics, BLOCK_CACHE_INDEX_HIT);
      }
      break;

    default:
      // TODO: introduce dedicated tickers/statistics/counters
      // for range tombstones
      if (get_context) {
        ++get_context->get_context_stats_.num_cache_data_hit;
      } else {
        RecordTick(statistics, BLOCK_CACHE_DATA_HIT);
      }
      break;
  }
}

void BlockBasedTable::UpdateCacheMissMetrics(BlockType block_type,
                                             GetContext* get_context) const {
  Statistics* const statistics = rep_->ioptions.stats;

  // TODO: introduce aggregate (not per-level) block cache miss count
  PERF_COUNTER_BY_LEVEL_ADD(block_cache_miss_count, 1,
                            static_cast<uint32_t>(rep_->level));

  if (get_context) {
    ++get_context->get_context_stats_.num_cache_miss;
  } else {
    RecordTick(statistics, BLOCK_CACHE_MISS);
  }

  // TODO: introduce perf counters for misses per block type
  switch (block_type) {
    case BlockType::kFilter:
    case BlockType::kFilterPartitionIndex:
      if (get_context) {
        ++get_context->get_context_stats_.num_cache_filter_miss;
      } else {
        RecordTick(statistics, BLOCK_CACHE_FILTER_MISS);
      }
      break;

    case BlockType::kCompressionDictionary:
      if (get_context) {
        ++get_context->get_context_stats_.num_cache_compression_dict_miss;
      } else {
        RecordTick(statistics, BLOCK_CACHE_COMPRESSION_DICT_MISS);
      }
      break;

    case BlockType::kIndex:
      if (get_context) {
        ++get_context->get_context_stats_.num_cache_index_miss;
      } else {
        RecordTick(statistics, BLOCK_CACHE_INDEX_MISS);
      }
      break;

    default:
      // TODO: introduce dedicated tickers/statistics/counters
      // for range tombstones
      if (get_context) {
        ++get_context->get_context_stats_.num_cache_data_miss;
      } else {
        RecordTick(statistics, BLOCK_CACHE_DATA_MISS);
      }
      break;
  }
}

void BlockBasedTable::UpdateCacheInsertionMetrics(
    BlockType block_type, GetContext* get_context, size_t usage, bool redundant,
    Statistics* const statistics) {
  // TODO: introduce perf counters for block cache insertions
  if (get_context) {
    ++get_context->get_context_stats_.num_cache_add;
    if (redundant) {
      ++get_context->get_context_stats_.num_cache_add_redundant;
    }
    get_context->get_context_stats_.num_cache_bytes_write += usage;
  } else {
    RecordTick(statistics, BLOCK_CACHE_ADD);
    if (redundant) {
      RecordTick(statistics, BLOCK_CACHE_ADD_REDUNDANT);
    }
    RecordTick(statistics, BLOCK_CACHE_BYTES_WRITE, usage);
  }

  switch (block_type) {
    case BlockType::kFilter:
    case BlockType::kFilterPartitionIndex:
      if (get_context) {
        ++get_context->get_context_stats_.num_cache_filter_add;
        if (redundant) {
          ++get_context->get_context_stats_.num_cache_filter_add_redundant;
        }
        get_context->get_context_stats_.num_cache_filter_bytes_insert += usage;
      } else {
        RecordTick(statistics, BLOCK_CACHE_FILTER_ADD);
        if (redundant) {
          RecordTick(statistics, BLOCK_CACHE_FILTER_ADD_REDUNDANT);
        }
        RecordTick(statistics, BLOCK_CACHE_FILTER_BYTES_INSERT, usage);
      }
      break;

    case BlockType::kCompressionDictionary:
      if (get_context) {
        ++get_context->get_context_stats_.num_cache_compression_dict_add;
        if (redundant) {
          ++get_context->get_context_stats_
                .num_cache_compression_dict_add_redundant;
        }
        get_context->get_context_stats_
            .num_cache_compression_dict_bytes_insert += usage;
      } else {
        RecordTick(statistics, BLOCK_CACHE_COMPRESSION_DICT_ADD);
        if (redundant) {
          RecordTick(statistics, BLOCK_CACHE_COMPRESSION_DICT_ADD_REDUNDANT);
        }
        RecordTick(statistics, BLOCK_CACHE_COMPRESSION_DICT_BYTES_INSERT,
                   usage);
      }
      break;

    case BlockType::kIndex:
      if (get_context) {
        ++get_context->get_context_stats_.num_cache_index_add;
        if (redundant) {
          ++get_context->get_context_stats_.num_cache_index_add_redundant;
        }
        get_context->get_context_stats_.num_cache_index_bytes_insert += usage;
      } else {
        RecordTick(statistics, BLOCK_CACHE_INDEX_ADD);
        if (redundant) {
          RecordTick(statistics, BLOCK_CACHE_INDEX_ADD_REDUNDANT);
        }
        RecordTick(statistics, BLOCK_CACHE_INDEX_BYTES_INSERT, usage);
      }
      break;

    default:
      // TODO: introduce dedicated tickers/statistics/counters
      // for range tombstones
      if (get_context) {
        ++get_context->get_context_stats_.num_cache_data_add;
        if (redundant) {
          ++get_context->get_context_stats_.num_cache_data_add_redundant;
        }
        get_context->get_context_stats_.num_cache_data_bytes_insert += usage;
      } else {
        RecordTick(statistics, BLOCK_CACHE_DATA_ADD);
        if (redundant) {
          RecordTick(statistics, BLOCK_CACHE_DATA_ADD_REDUNDANT);
        }
        RecordTick(statistics, BLOCK_CACHE_DATA_BYTES_INSERT, usage);
      }
      break;
  }
}

Status BlockBasedTable::KVSepBptreeDecodePointer(const Slice& ptr,
                                                BlockHandle* value_block_handle,
                                                uint32_t* value_off,
                                                uint32_t* value_len) {
  return DecodeKVSepBptreeLeafPointer(ptr, value_block_handle, value_off,
                                      value_len);
}

Status BlockBasedTable::KVSepBptreeGetValueBlock(
    const ReadOptions& ro, const BlockHandle& value_block_handle,
    CachableEntry<Block_kKVSepValue>* value_block,
    BlockCacheLookupContext* lookup_context, FilePrefetchBuffer* prefetch_buffer) const {
  if (value_block == nullptr) {
    return Status::InvalidArgument("kvsep value block: null output");
  }
  value_block->Reset();
  if (value_block_handle.IsNull()) {
    return Status::OK();
  }

  Status s;
  s = RetrieveBlock<Block_kKVSepValue>(
      prefetch_buffer, ro, value_block_handle,
      rep_->decompressor.get(),
      value_block, /*get_context=*/nullptr, lookup_context,
      /*for_compaction=*/false, /*use_cache=*/ro.fill_cache, /*async_read=*/false,
      /*use_block_cache_for_lookup=*/true);
  return s;
}

namespace {
// Return True if table_properties has `user_prop_name` has a `true` value
// or it doesn't contain this property (for backward compatible).
bool IsFeatureSupported(const TableProperties& table_properties,
                        const std::string& user_prop_name, Logger* info_log) {
  auto& props = table_properties.user_collected_properties;
  auto pos = props.find(user_prop_name);
  // Older version doesn't have this value set. Skip this check.
  if (pos != props.end()) {
    if (pos->second == kPropFalse) {
      return false;
    } else if (pos->second != kPropTrue) {
      ROCKS_LOG_WARN(info_log, "Property %s has invalidate value %s",
                     user_prop_name.c_str(), pos->second.c_str());
    }
  }
  return true;
}

// Caller has to ensure seqno is not nullptr.
// Set *seqno to the global sequence number for reading this file.
Status GetGlobalSequenceNumber(const TableProperties& table_properties,
                               SequenceNumber largest_seqno,
                               SequenceNumber* seqno) {
  const auto& props = table_properties.user_collected_properties;
  const auto version_pos = props.find(ExternalSstFilePropertyNames::kVersion);
  const auto seqno_pos = props.find(ExternalSstFilePropertyNames::kGlobalSeqno);

  *seqno = kDisableGlobalSequenceNumber;
  if (version_pos == props.end()) {
    if (seqno_pos != props.end()) {
      std::array<char, 200> msg_buf;
      // This is not an external sst file, global_seqno is not supported.
      snprintf(
          msg_buf.data(), msg_buf.max_size(),
          "A non-external sst file have global seqno property with value %s",
          seqno_pos->second.c_str());
      return Status::Corruption(msg_buf.data());
    }
    return Status::OK();
  }

  uint32_t version = DecodeFixed32(version_pos->second.c_str());
  if (version != 2) {
    std::array<char, 200> msg_buf;
    if (version != 1) {
      snprintf(msg_buf.data(), msg_buf.max_size(),
               "An external sst file has corrupted version %u.", version);
      return Status::Corruption(msg_buf.data());
    }
    if (seqno_pos != props.end()) {
      // This is a v1 external sst file, global_seqno is not supported.
      snprintf(msg_buf.data(), msg_buf.max_size(),
               "An external sst file with version %u has global seqno "
               "property with value %s",
               version, seqno_pos->second.c_str());
      return Status::Corruption(msg_buf.data());
    }
    return Status::OK();
  }

  // Since we have a plan to deprecate global_seqno, we do not return failure
  // if seqno_pos == props.end(). We rely on version_pos to detect whether the
  // SST is external.
  SequenceNumber global_seqno(0);
  if (seqno_pos != props.end()) {
    global_seqno = DecodeFixed64(seqno_pos->second.c_str());
  }
  // SstTableReader open table reader with kMaxSequenceNumber as largest_seqno
  // to denote it is unknown.
  if (largest_seqno < kMaxSequenceNumber) {
    if (global_seqno == 0) {
      global_seqno = largest_seqno;
    }
    if (global_seqno != largest_seqno) {
      std::array<char, 200> msg_buf;
      snprintf(
          msg_buf.data(), msg_buf.max_size(),
          "An external sst file with version %u have global seqno property "
          "with value %s, while largest seqno in the file is %llu",
          version, seqno_pos->second.c_str(),
          static_cast<unsigned long long>(largest_seqno));
      return Status::Corruption(msg_buf.data());
    }
  }
  *seqno = global_seqno;

  if (global_seqno > kMaxSequenceNumber) {
    std::array<char, 200> msg_buf;
    snprintf(msg_buf.data(), msg_buf.max_size(),
             "An external sst file with version %u have global seqno property "
             "with value %llu, which is greater than kMaxSequenceNumber",
             version, static_cast<unsigned long long>(global_seqno));
    return Status::Corruption(msg_buf.data());
  }

  return Status::OK();
}

Status GetDecompressor(const std::string& compression_name,
                       UnownedPtr<CompressionManager> compression_manager,
                       uint32_t table_format_version,
                       std::shared_ptr<Decompressor>* out_decompressor) {
  if (compression_name.empty()) {
    // Very old file (before RocksDB 4.9.0) that might contain compressed
    // blocks. Get a general decompressor for the format version.
    auto mgr_to_use = GetBuiltinCompressionManager(
        GetCompressFormatForVersion(table_format_version));
    *out_decompressor = mgr_to_use->GetDecompressor();
    return Status::OK();
  }
  if (FormatVersionUsesCompressionManagerName(table_format_version)) {
    constexpr char kFieldSep = ';';
    size_t separator_pos = compression_name.find_first_of(kFieldSep);
    if (separator_pos == std::string::npos) {
      return Status::Corruption(
          "Missing separator in compression_name property");
    }
    // Built with explicit CompressionManager and schema support for
    // identifying its compatibility name, which is the first field here.
    Slice compatibility_name(compression_name.data(), separator_pos);
    std::shared_ptr<CompressionManager> mgr_to_use;
    if (compression_manager) {
      // First attempt to go through the compression manager configured for
      // writing new files, for efficiency (usually correct) and not forcing
      // use of ObjectLibrary registration (dependency injection).
      mgr_to_use = compression_manager->FindCompatibleCompressionManager(
          compatibility_name);
    }
    if (mgr_to_use == nullptr) {
      ConfigOptions strict;
      strict.ignore_unknown_options = false;
      strict.ignore_unsupported_options = false;
      Status s = CompressionManager::CreateFromString(
          strict, compatibility_name.ToString(), &mgr_to_use);
      // Even though we might be able to recover from "not found" if only
      // built-in compression types are used (would be checked below), it
      // would provide misleading or unreliable success to allow that to
      // succeed.
      if (!s.ok()) {
        return s;
      }
      assert(mgr_to_use || compatibility_name == kNullptrString ||
             compatibility_name.empty());
    }

    // Second field is set of compression types actually used in the file
    size_t start_pos = separator_pos + 1;
    separator_pos = compression_name.find_first_of(kFieldSep, start_pos);
    if (UNLIKELY(separator_pos == std::string::npos)) {
      return Status::Corruption("Missing second field from compression_name");
    }
    if (UNLIKELY((separator_pos - start_pos) & 1)) {
      return Status::Corruption(
          "Second field of compression_name has odd size");
    }
    size_t count = (separator_pos - start_pos) / 2;
    auto ctypes = std::make_unique<CompressionType[]>(count);
    const char* ptr = compression_name.data() + start_pos;
    for (size_t i = 0; i < count; ++i) {
      uint64_t val = 0;
      bool success = ParseBaseChars<16>(&ptr, 2, &val);
      if (UNLIKELY(!success || val == kNoCompression ||
                   val >= kDisableCompressionOption)) {
        return Status::Corruption(
            "Error parsing second field of compression_name");
      }
      ctypes[i] = static_cast<CompressionType>(val);
    }
    if (mgr_to_use) {
      *out_decompressor = mgr_to_use->GetDecompressorForTypes(
          ctypes.get(), ctypes.get() + count);
      assert(*out_decompressor || count == 0);
    } else {
      // Compression/decompression disabled
      *out_decompressor = nullptr;
      assert(count == 0);
    }
    // Can ignore possible additional future fields
  } else {
    // No explicit CompressionManager, e.g. legacy file support where
    // decompressing with built-in CompressionManager works.
    CompressionType saved_comp_type =
        CompressionTypeFromString(compression_name);
    if (saved_comp_type == kDisableCompressionOption) {
      // Unrecognized. For RocksDB versions able to read format_version=7,
      // this is considered an error so that we can continue to evolve the
      // schema of the compression_name property and report good error
      // messages.
      return Status::Corruption("Unrecognized compression_name: " +
                                compression_name);
    } else if (saved_comp_type != kNoCompression) {
      // Use built-in compression manager
      auto mgr_to_use = GetBuiltinCompressionManager(
          GetCompressFormatForVersion(table_format_version));
      *out_decompressor =
          mgr_to_use->GetDecompressorOptimizeFor(saved_comp_type);
    } else {
      // No compression -> decompressor not needed
      *out_decompressor = nullptr;
    }
  }
  return Status::OK();
}
}  // namespace

void BlockBasedTable::SetupBaseCacheKey(const TableProperties* properties,
                                        const std::string& cur_db_session_id,
                                        uint64_t cur_file_number,
                                        OffsetableCacheKey* out_base_cache_key,
                                        bool* out_is_stable) {
  // Use a stable cache key if sufficient data is in table properties
  std::string db_session_id;
  uint64_t file_num;
  std::string db_id;
  if (properties && !properties->db_session_id.empty() &&
      properties->orig_file_number > 0) {
    // (Newer SST file case)
    // We must have both properties to get a stable unique id because
    // CreateColumnFamilyWithImport or IngestExternalFiles can change the
    // file numbers on a file.
    db_session_id = properties->db_session_id;
    file_num = properties->orig_file_number;
    // Less critical, populated in earlier release than above
    db_id = properties->db_id;
    if (out_is_stable) {
      *out_is_stable = true;
    }
  } else {
    // (Old SST file case)
    // We use (unique) cache keys based on current identifiers. These are at
    // least stable across table file close and re-open, but not across
    // different DBs nor DB close and re-open.
    db_session_id = cur_db_session_id;
    file_num = cur_file_number;
    // Plumbing through the DB ID to here would be annoying, and of limited
    // value because of the case of VersionSet::Recover opening some table
    // files and later setting the DB ID. So we just rely on uniqueness
    // level provided by session ID.
    db_id = "unknown";
    if (out_is_stable) {
      *out_is_stable = false;
    }
  }

  // Too many tests to update to get these working
  // assert(file_num > 0);
  // assert(!db_session_id.empty());
  // assert(!db_id.empty());

  // Minimum block size is 5 bytes; therefore we can trim off two lower bits
  // from offsets. See GetCacheKey.
  *out_base_cache_key = OffsetableCacheKey(db_id, db_session_id, file_num);
}

CacheKey BlockBasedTable::GetCacheKey(const OffsetableCacheKey& base_cache_key,
                                      const BlockHandle& handle) {
  // Minimum block size is 5 bytes; therefore we can trim off two lower bits
  // from offet.
  return base_cache_key.WithOffset(handle.offset() >> 2);
}

Status BlockBasedTable::Open(
    const ReadOptions& read_options, const ImmutableOptions& ioptions,
    const EnvOptions& env_options, const BlockBasedTableOptions& table_options,
    const InternalKeyComparator& internal_comparator,
    std::unique_ptr<RandomAccessFileReader>&& file, uint64_t file_size,
    uint8_t block_protection_bytes_per_key,
    std::unique_ptr<TableReader>* table_reader, uint64_t tail_size,
    std::shared_ptr<CacheReservationManager> table_reader_cache_res_mgr,
    const std::shared_ptr<const SliceTransform>& prefix_extractor,
    UnownedPtr<CompressionManager> compression_manager,
    const bool prefetch_index_and_filter_in_cache, const bool skip_filters,
    const int level, const bool immortal_table,
    const SequenceNumber largest_seqno, const bool force_direct_prefetch,
    TailPrefetchStats* tail_prefetch_stats,
    BlockCacheTracer* const block_cache_tracer,
    size_t max_file_size_for_l0_meta_pin, const std::string& cur_db_session_id,
    uint64_t cur_file_num, UniqueId64x2 expected_unique_id,
    const bool user_defined_timestamps_persisted) {
  table_reader->reset();

  Status s;
  Footer footer;
  std::unique_ptr<FilePrefetchBuffer> prefetch_buffer;

  // From read_options, retain deadline, io_timeout, rate_limiter_priority, and
  // verify_checksums. In future, we may retain more options.
  // TODO: audit more ReadOptions and do this in a way that brings attention
  // on new ReadOptions?
  ReadOptions ro;
  ro.deadline = read_options.deadline;
  ro.io_timeout = read_options.io_timeout;
  ro.rate_limiter_priority = read_options.rate_limiter_priority;
  ro.verify_checksums = read_options.verify_checksums;
  ro.io_activity = read_options.io_activity;
  ro.fill_cache = read_options.fill_cache;

  // prefetch both index and filters, down to all partitions
  const bool prefetch_all = prefetch_index_and_filter_in_cache || level == 0;
  const bool preload_all = !table_options.cache_index_and_filter_blocks;

  if (!ioptions.allow_mmap_reads && !env_options.use_mmap_reads) {
    s = PrefetchTail(ro, ioptions, file.get(), file_size, force_direct_prefetch,
                     tail_prefetch_stats, prefetch_all, preload_all,
                     &prefetch_buffer, ioptions.stats, tail_size,
                     ioptions.logger);
    // Return error in prefetch path to users.
    if (!s.ok()) {
      return s;
    }
  } else {
    // Should not prefetch for mmap mode.
    prefetch_buffer.reset(new FilePrefetchBuffer(
        ReadaheadParams(), false /* enable */, true /* track_min_offset */));
  }

  // Read in the following order:
  //    1. Footer
  //    2. [metaindex block]
  //    3. [meta block: properties]
  //    4. [meta block: range deletion tombstone]
  //    5. [meta block: compression dictionary]
  //    6. [meta block: index]
  //    7. [meta block: filter]
  IOOptions opts;
  IODebugContext dbg;
  s = file->PrepareIOOptions(ro, opts, &dbg);
  if (s.ok()) {
    s = ReadFooterFromFile(opts, file.get(), *ioptions.fs,
                           prefetch_buffer.get(), file_size, &footer,
                           kBlockBasedTableMagicNumber, ioptions.stats);
  }
  if (!s.ok()) {
    if (s.IsCorruption()) {
      RecordTick(ioptions.statistics.get(), SST_FOOTER_CORRUPTION_COUNT);
    }
    return s;
  }
  if (!IsSupportedFormatVersion(footer.format_version()) &&
      !TEST_AllowUnsupportedFormatVersion()) {
    return Status::Corruption(
        "Unknown Footer version. Maybe this file was created with newer "
        "version of RocksDB?");
  }

  BlockCacheLookupContext lookup_context{TableReaderCaller::kPrefetch};
  Rep* rep = new BlockBasedTable::Rep(
      ioptions, env_options, table_options, internal_comparator, skip_filters,
      file_size, level, immortal_table, user_defined_timestamps_persisted);
  rep->file = std::move(file);
  rep->footer = footer;

  // Some ancient versions (~2.5 - 2.7, format_version=1) could compress the
  // metaindex block, so we need to allow for that
  if (footer.format_version() < 2) {
    auto mgr = GetBuiltinCompressionManager(/*compression_format_version=*/1);
    rep->decompressor = mgr->GetDecompressor();
  }

  // For fully portable/stable cache keys, we need to read the properties
  // block before setting up cache keys. TODO: consider setting up a bootstrap
  // cache key for PersistentCache to use for metaindex and properties blocks.
  rep->persistent_cache_options = PersistentCacheOptions();

  // Meta-blocks are not dictionary compressed. Explicitly set the dictionary
  // handle to null, otherwise it may be seen as uninitialized during the below
  // meta-block reads.
  rep->compression_dict_handle = BlockHandle::NullBlockHandle();

  rep->create_context.protection_bytes_per_key = block_protection_bytes_per_key;
  // Read metaindex
  std::unique_ptr<BlockBasedTable> new_table(
      new BlockBasedTable(rep, block_cache_tracer));
  std::unique_ptr<Block> metaindex;
  std::unique_ptr<InternalIterator> metaindex_iter;
  s = new_table->ReadMetaIndexBlock(ro, prefetch_buffer.get(), &metaindex,
                                    &metaindex_iter);
  if (!s.ok()) {
    return s;
  }

  // Populates table_properties and some fields that depend on it,
  // such as index_type.
  s = new_table->ReadPropertiesBlock(ro, prefetch_buffer.get(),
                                     metaindex_iter.get(), largest_seqno);
  if (!s.ok()) {
    return s;
  }

  // Read compression metadata and configure decompressor
  s = GetDecompressor(
      rep->table_properties ? rep->table_properties->compression_name
                            : std::string{},
      compression_manager, footer.format_version(), &rep->decompressor);
  if (!s.ok()) {
    return s;
  }

  // Populate BlockCreateContext
  rep->create_context = BlockCreateContext(
      &rep->table_options, &rep->ioptions, rep->ioptions.stats,
      rep->decompressor.get(), block_protection_bytes_per_key,
      rep->internal_comparator.user_comparator(), rep->index_value_is_full,
      rep->index_has_first_key);

  // Experimental: KV-separation + B+tree indexing.
  // The encoding is self-contained in the leaf entries (value pointers contain
  // value-block handles), so there is no required meta mapping to load here.
  if (rep->table_properties) {
    const auto& u = rep->table_properties->user_collected_properties;
    auto it = u.find(kKVSepBptreeTablePropertyKey);
    if (it != u.end() && it->second == "1") {
      rep->experimental_kvsep_bptree_enabled = true;
    }
    auto it_leaf_fmt = u.find(kKVSepBptreeLeafFormatVersionPropertyKey);
    if (it_leaf_fmt != u.end()) {
      try {
        uint32_t v = ParseUint32(it_leaf_fmt->second);
        if (v >= 1) {
          rep->kvsep_bptree_leaf_format_version = v;
        }
      } catch (...) {
        // Ignore malformed property and keep default.
      }
    }
    auto it_levels = u.find(kKVSepBptreeIndexLevelsPropertyKey);
    if (it_levels != u.end()) {
      try {
        uint32_t levels = ParseUint32(it_levels->second);
        if (levels >= 1) {
          rep->kvsep_bptree_index_levels = levels;
        }
      } catch (...) {
        // Ignore malformed property and keep default.
      }
    }
  }

  // Check expected unique id if provided
  if (expected_unique_id != kNullUniqueId64x2) {
    auto props = rep->table_properties;
    if (!props) {
      return Status::Corruption("Missing table properties on file " +
                                std::to_string(cur_file_num) +
                                " with known unique ID");
    }
    UniqueId64x2 actual_unique_id{};
    s = GetSstInternalUniqueId(props->db_id, props->db_session_id,
                               props->orig_file_number, &actual_unique_id,
                               /*force*/ true);
    assert(s.ok());  // because force=true
    if (expected_unique_id != actual_unique_id) {
      return Status::Corruption(
          "Mismatch in unique ID on table file " +
          std::to_string(cur_file_num) +
          ". Expected: " + InternalUniqueIdToHumanString(&expected_unique_id) +
          " Actual: " + InternalUniqueIdToHumanString(&actual_unique_id));
    }
    TEST_SYNC_POINT_CALLBACK("BlockBasedTable::Open::PassedVerifyUniqueId",
                             &actual_unique_id);
  } else {
    TEST_SYNC_POINT_CALLBACK("BlockBasedTable::Open::SkippedVerifyUniqueId",
                             nullptr);
    if (ioptions.verify_sst_unique_id_in_manifest && ioptions.logger) {
      // A crude but isolated way of reporting unverified files. This should not
      // be an ongoing concern so doesn't deserve a place in Statistics IMHO.
      static std::atomic<uint64_t> unverified_count{0};
      auto prev_count =
          unverified_count.fetch_add(1, std::memory_order_relaxed);
      if (prev_count == 0) {
        ROCKS_LOG_WARN(
            ioptions.logger,
            "At least one SST file opened without unique ID to verify: %" PRIu64
            ".sst",
            cur_file_num);
      } else if (prev_count % 1000 == 0) {
        ROCKS_LOG_WARN(
            ioptions.logger,
            "Another ~1000 SST files opened without unique ID to verify");
      }
    }
  }

  // Set up prefix extracto as needed
  bool force_null_table_prefix_extractor = false;
  TEST_SYNC_POINT_CALLBACK(
      "BlockBasedTable::Open::ForceNullTablePrefixExtractor",
      &force_null_table_prefix_extractor);
  if (force_null_table_prefix_extractor) {
    assert(!rep->table_prefix_extractor);
  } else if (!PrefixExtractorChangedHelper(rep->table_properties.get(),
                                           prefix_extractor.get())) {
    // Establish fast path for unchanged prefix_extractor
    rep->table_prefix_extractor = prefix_extractor;
  } else {
    // Current prefix_extractor doesn't match table
    if (rep->table_properties) {
      //**TODO: If/When the DBOptions has a registry in it, the ConfigOptions
      // will need to use it
      ConfigOptions config_options;
      Status st = SliceTransform::CreateFromString(
          config_options, rep->table_properties->prefix_extractor_name,
          &(rep->table_prefix_extractor));
      if (!st.ok()) {
        //**TODO: Should this be error be returned or swallowed?
        ROCKS_LOG_ERROR(rep->ioptions.logger,
                        "Failed to create prefix extractor[%s]: %s",
                        rep->table_properties->prefix_extractor_name.c_str(),
                        st.ToString().c_str());
      }
    }
  }

  // With properties loaded, we can set up portable/stable cache keys
  SetupBaseCacheKey(rep->table_properties.get(), cur_db_session_id,
                    cur_file_num, &rep->base_cache_key);

  rep->persistent_cache_options =
      PersistentCacheOptions(rep->table_options.persistent_cache,
                             rep->base_cache_key, rep->ioptions.stats);

  s = new_table->ReadRangeDelBlock(ro, prefetch_buffer.get(),
                                   metaindex_iter.get(), internal_comparator,
                                   &lookup_context);
  if (!s.ok()) {
    return s;
  }
  rep->verify_checksum_set_on_open = ro.verify_checksums;
  s = new_table->PrefetchIndexAndFilterBlocks(
      ro, prefetch_buffer.get(), metaindex_iter.get(), new_table.get(),
      prefetch_all, table_options, level, file_size,
      max_file_size_for_l0_meta_pin, &lookup_context);

  if (s.ok()) {
    // Update tail prefetch stats
    assert(prefetch_buffer.get() != nullptr);
    if (tail_prefetch_stats != nullptr) {
      assert(prefetch_buffer->min_offset_read() < file_size);
      tail_prefetch_stats->RecordEffectiveSize(
          static_cast<size_t>(file_size) - prefetch_buffer->min_offset_read());
    }
  }

  if (s.ok() && table_reader_cache_res_mgr) {
    std::size_t mem_usage = new_table->ApproximateMemoryUsage();
    s = table_reader_cache_res_mgr->MakeCacheReservation(
        mem_usage, &(rep->table_reader_cache_res_handle));
    if (s.IsMemoryLimit()) {
      s = Status::MemoryLimit(
          "Can't allocate " +
          kCacheEntryRoleToCamelString[static_cast<std::uint32_t>(
              CacheEntryRole::kBlockBasedTableReader)] +
          " due to memory limit based on "
          "cache capacity for memory allocation");
    }
  }

  if (s.ok()) {
    *table_reader = std::move(new_table);
  }
  return s;
}

Status BlockBasedTable::PrefetchTail(
    const ReadOptions& ro, const ImmutableOptions& ioptions,
    RandomAccessFileReader* file, uint64_t file_size,
    bool force_direct_prefetch, TailPrefetchStats* tail_prefetch_stats,
    const bool prefetch_all, const bool preload_all,
    std::unique_ptr<FilePrefetchBuffer>* prefetch_buffer, Statistics* stats,
    uint64_t tail_size, Logger* const logger) {
  assert(tail_size <= file_size);

  size_t tail_prefetch_size = 0;
  if (tail_size != 0) {
    tail_prefetch_size = tail_size;
  } else {
    // Fallback for SST files, for which tail size is not recorded in the
    // manifest. Eventually, this fallback might be removed, so it's
    // better to make sure that such SST files get compacted.
    // See https://github.com/facebook/rocksdb/issues/12664
    if (tail_prefetch_stats != nullptr) {
      // Multiple threads may get a 0 (no history) when running in parallel,
      // but it will get cleared after the first of them finishes.
      tail_prefetch_size = tail_prefetch_stats->GetSuggestedPrefetchSize();
    }
    if (tail_prefetch_size == 0) {
      // Before read footer, readahead backwards to prefetch data. Do more
      // readahead if we're going to read index/filter.
      // TODO: This may incorrectly select small readahead in case partitioned
      // index/filter is enabled and top-level partition pinning is enabled.
      // That's because we need to issue readahead before we read the
      // properties, at which point we don't yet know the index type.
      tail_prefetch_size = prefetch_all || preload_all ? 512 * 1024 : 4 * 1024;

      ROCKS_LOG_WARN(
          logger,
          "[%s] Tail prefetch size %zu is calculated based on heuristics.",
          file->file_name().c_str(), tail_prefetch_size);
    } else {
      ROCKS_LOG_WARN(logger,
                     "[%s] Tail prefetch size %zu is calculated based on "
                     "TailPrefetchStats.",
                     file->file_name().c_str(), tail_prefetch_size);
    }
    TEST_SYNC_POINT("BlockBasedTable::PrefetchTail::TaiSizeNotRecorded");
  }
  size_t prefetch_off;
  size_t prefetch_len;
  if (file_size < tail_prefetch_size) {
    prefetch_off = 0;
    prefetch_len = static_cast<size_t>(file_size);
  } else {
    prefetch_off = static_cast<size_t>(file_size - tail_prefetch_size);
    prefetch_len = tail_prefetch_size;
  }

#ifndef NDEBUG
  std::pair<size_t*, size_t*> prefetch_off_len_pair = {&prefetch_off,
                                                       &prefetch_len};
  TEST_SYNC_POINT_CALLBACK("BlockBasedTable::Open::TailPrefetchLen",
                           &prefetch_off_len_pair);
#endif  // NDEBUG

  IOOptions opts;
  IODebugContext dbg;
  Status s = file->PrepareIOOptions(ro, opts, &dbg);
  // Try file system prefetch
  if (s.ok() && !file->use_direct_io() && !force_direct_prefetch) {
    if (!file->Prefetch(opts, prefetch_off, prefetch_len).IsNotSupported()) {
      prefetch_buffer->reset(new FilePrefetchBuffer(
          ReadaheadParams(), false /* enable */, true /* track_min_offset */));
      return Status::OK();
    }
  }

  // Use `FilePrefetchBuffer`
  prefetch_buffer->reset(new FilePrefetchBuffer(
      ReadaheadParams(), true /* enable */, true /* track_min_offset */,
      ioptions.fs.get() /* fs */, nullptr /* clock */, stats,
      /* readahead_cb */ nullptr,
      FilePrefetchBufferUsage::kTableOpenPrefetchTail));

  if (s.ok()) {
    s = (*prefetch_buffer)->Prefetch(opts, file, prefetch_off, prefetch_len);
  }
  return s;
}

Status BlockBasedTable::ReadPropertiesBlock(
    const ReadOptions& ro, FilePrefetchBuffer* prefetch_buffer,
    InternalIterator* meta_iter, const SequenceNumber largest_seqno) {
  Status s;
  BlockHandle handle;
  s = FindOptionalMetaBlock(meta_iter, kPropertiesBlockName, &handle);

  if (!s.ok()) {
    ROCKS_LOG_WARN(rep_->ioptions.logger,
                   "Error when seeking to properties block from file: %s",
                   s.ToString().c_str());
  } else if (!handle.IsNull()) {
    s = meta_iter->status();
    std::unique_ptr<TableProperties> table_properties;
    if (s.ok()) {
      s = ReadTablePropertiesHelper(
          ro, handle, rep_->file.get(), prefetch_buffer, rep_->footer,
          rep_->ioptions, &table_properties, nullptr /* memory_allocator */);
    }
    IGNORE_STATUS_IF_ERROR(s);

    if (!s.ok()) {
      ROCKS_LOG_WARN(rep_->ioptions.logger,
                     "Encountered error while reading data from properties "
                     "block %s",
                     s.ToString().c_str());
    } else {
      assert(table_properties != nullptr);
      rep_->table_properties = std::move(table_properties);

      if (s.ok()) {
        s = rep_->seqno_to_time_mapping.DecodeFrom(
            rep_->table_properties->seqno_to_time_mapping);
      }
      if (!s.ok()) {
        ROCKS_LOG_WARN(
            rep_->ioptions.logger,
            "Problem reading or processing seqno-to-time mapping: %s",
            s.ToString().c_str());
      }
    }
  } else {
    ROCKS_LOG_ERROR(rep_->ioptions.logger,
                    "Cannot find Properties block from file.");
  }

  // Read the table properties, if provided.
  if (rep_->table_properties) {
    rep_->whole_key_filtering &=
        IsFeatureSupported(*(rep_->table_properties),
                           BlockBasedTablePropertyNames::kWholeKeyFiltering,
                           rep_->ioptions.logger);
    rep_->prefix_filtering &= IsFeatureSupported(
        *(rep_->table_properties),
        BlockBasedTablePropertyNames::kPrefixFiltering, rep_->ioptions.logger);

    rep_->index_key_includes_seq =
        rep_->table_properties->index_key_is_user_key == 0;
    rep_->index_value_is_full =
        rep_->table_properties->index_value_is_delta_encoded == 0;

    // Update index_type with the true type.
    // If table properties don't contain index type, we assume that the table
    // is in very old format and has kBinarySearch index type.
    auto& props = rep_->table_properties->user_collected_properties;
    auto index_type_pos = props.find(BlockBasedTablePropertyNames::kIndexType);
    if (index_type_pos != props.end()) {
      rep_->index_type = static_cast<BlockBasedTableOptions::IndexType>(
          DecodeFixed32(index_type_pos->second.c_str()));
    }
    auto min_ts_pos = props.find("rocksdb.timestamp_min");
    if (min_ts_pos != props.end()) {
      rep_->min_timestamp = Slice(min_ts_pos->second);
    }
    auto max_ts_pos = props.find("rocksdb.timestamp_max");
    if (max_ts_pos != props.end()) {
      rep_->max_timestamp = Slice(max_ts_pos->second);
    }

    rep_->index_has_first_key =
        rep_->index_type == BlockBasedTableOptions::kBinarySearchWithFirstKey;

    s = GetGlobalSequenceNumber(*(rep_->table_properties), largest_seqno,
                                &(rep_->global_seqno));
    if (!s.ok()) {
      ROCKS_LOG_ERROR(rep_->ioptions.logger, "%s", s.ToString().c_str());
    }
  }
  return s;
}

Status BlockBasedTable::ReadRangeDelBlock(
    const ReadOptions& read_options, FilePrefetchBuffer* prefetch_buffer,
    InternalIterator* meta_iter,
    const InternalKeyComparator& internal_comparator,
    BlockCacheLookupContext* lookup_context) {
  Status s;
  BlockHandle range_del_handle;
  s = FindOptionalMetaBlock(meta_iter, kRangeDelBlockName, &range_del_handle);
  if (!s.ok()) {
    ROCKS_LOG_WARN(
        rep_->ioptions.logger,
        "Error when seeking to range delete tombstones block from file: %s",
        s.ToString().c_str());
  } else if (!range_del_handle.IsNull()) {
    Status tmp_status;
    std::unique_ptr<InternalIterator> iter(NewDataBlockIterator<DataBlockIter>(
        read_options, range_del_handle,
        /*input_iter=*/nullptr, BlockType::kRangeDeletion,
        /*get_context=*/nullptr, lookup_context, prefetch_buffer,
        /*for_compaction= */ false, /*async_read= */ false, tmp_status,
        /*use_block_cache_for_lookup=*/true));
    assert(iter != nullptr);
    s = iter->status();
    if (!s.ok()) {
      ROCKS_LOG_WARN(
          rep_->ioptions.logger,
          "Encountered error while reading data from range del block %s",
          s.ToString().c_str());
      IGNORE_STATUS_IF_ERROR(s);
    } else {
      std::vector<SequenceNumber> snapshots;
      // When user defined timestamps are not persisted, the range tombstone end
      // key read from the data block doesn't include user timestamp.
      // The range tombstone start key should already include user timestamp as
      // it's handled at block parsing level in the same way as the other data
      // blocks.
      rep_->fragmented_range_dels =
          std::make_shared<FragmentedRangeTombstoneList>(
              std::move(iter), internal_comparator, false /*for_compaction=*/,
              snapshots, rep_->user_defined_timestamps_persisted);
    }
  }
  return s;
}

Status BlockBasedTable::PrefetchIndexAndFilterBlocks(
    const ReadOptions& ro, FilePrefetchBuffer* prefetch_buffer,
    InternalIterator* meta_iter, BlockBasedTable* new_table, bool prefetch_all,
    const BlockBasedTableOptions& table_options, const int level,
    size_t file_size, size_t max_file_size_for_l0_meta_pin,
    BlockCacheLookupContext* lookup_context) {
  // Find filter handle and filter type
  if (rep_->filter_policy) {
    auto name = rep_->filter_policy->CompatibilityName();
    for (const auto& [filter_type, prefix] :
         {std::make_pair(Rep::FilterType::kFullFilter, kFullFilterBlockPrefix),
          std::make_pair(Rep::FilterType::kPartitionedFilter,
                         kPartitionedFilterBlockPrefix),
          std::make_pair(Rep::FilterType::kNoFilter,
                         kObsoleteFilterBlockPrefix)}) {
      std::string filter_block_key = prefix + name;
      if (FindMetaBlock(meta_iter, filter_block_key, &rep_->filter_handle)
              .ok()) {
        rep_->filter_type = filter_type;
        if (filter_type == Rep::FilterType::kNoFilter) {
          ROCKS_LOG_WARN(
              rep_->ioptions.logger,
              "Detected obsolete filter type in %s. Read performance might "
              "suffer until DB is fully re-compacted.",
              rep_->file->file_name().c_str());
        }
        break;
      }
    }
  }
  // Partition filters cannot be enabled without partition indexes
  assert(rep_->filter_type != Rep::FilterType::kPartitionedFilter ||
         rep_->index_type == BlockBasedTableOptions::kTwoLevelIndexSearch);

  // Find compression dictionary handle
  Status s = FindOptionalMetaBlock(meta_iter, kCompressionDictBlockName,
                                   &rep_->compression_dict_handle);
  if (!s.ok()) {
    return s;
  }

  BlockBasedTableOptions::IndexType index_type = rep_->index_type;

  const bool use_cache = table_options.cache_index_and_filter_blocks;

  const bool maybe_flushed =
      level == 0 && file_size <= max_file_size_for_l0_meta_pin;
  std::function<bool(PinningTier, PinningTier)> is_pinned =
      [maybe_flushed, &is_pinned](PinningTier pinning_tier,
                                  PinningTier fallback_pinning_tier) {
        // Fallback to fallback would lead to infinite recursion. Disallow it.
        assert(fallback_pinning_tier != PinningTier::kFallback);

        switch (pinning_tier) {
          case PinningTier::kFallback:
            return is_pinned(fallback_pinning_tier,
                             PinningTier::kNone /* fallback_pinning_tier */);
          case PinningTier::kNone:
            return false;
          case PinningTier::kFlushedAndSimilar:
            return maybe_flushed;
          case PinningTier::kAll:
            return true;
        };

        // In GCC, this is needed to suppress `control reaches end of non-void
        // function [-Werror=return-type]`.
        assert(false);
        return false;
      };
  const bool pin_top_level_index = is_pinned(
      table_options.metadata_cache_options.top_level_index_pinning,
      table_options.pin_top_level_index_and_filter ? PinningTier::kAll
                                                   : PinningTier::kNone);
  const bool pin_partition =
      is_pinned(table_options.metadata_cache_options.partition_pinning,
                table_options.pin_l0_filter_and_index_blocks_in_cache
                    ? PinningTier::kFlushedAndSimilar
                    : PinningTier::kNone);
  const bool pin_unpartitioned =
      is_pinned(table_options.metadata_cache_options.unpartitioned_pinning,
                table_options.pin_l0_filter_and_index_blocks_in_cache
                    ? PinningTier::kFlushedAndSimilar
                    : PinningTier::kNone);

  // pin the first level of index
  const bool pin_index =
      index_type == BlockBasedTableOptions::kTwoLevelIndexSearch
          ? pin_top_level_index
          : pin_unpartitioned;
  // prefetch the first level of index
  // WART: this might be redundant (unnecessary cache hit) if !pin_index,
  // depending on prepopulate_block_cache option
  const bool prefetch_index = prefetch_all || pin_index;

  std::unique_ptr<IndexReader> index_reader;
  s = new_table->CreateIndexReader(ro, prefetch_buffer, meta_iter, use_cache,
                                   prefetch_index, pin_index, lookup_context,
                                   &index_reader);
  if (!s.ok()) {
    return s;
  }
  if (table_options.user_defined_index_factory != nullptr) {
    std::string udi_name(table_options.user_defined_index_factory->Name());
    BlockHandle udi_block_handle;

    // Should we use FindOptionalMetaBlock here?
    s = FindMetaBlock(meta_iter, kUserDefinedIndexPrefix + udi_name,
                      &udi_block_handle);
    if (!s.ok()) {
      RecordTick(rep_->ioptions.statistics.get(),
                 SST_USER_DEFINED_INDEX_LOAD_FAIL_COUNT);
      if (table_options.fail_if_no_udi_on_open) {
        ROCKS_LOG_ERROR(rep_->ioptions.logger,
                        "Failed to find the the UDI block %s in file %s; %s",
                        udi_name.c_str(), rep_->file->file_name().c_str(),
                        s.ToString().c_str());
        // MAke the status more informative
        s = Status::Corruption(s.ToString(), rep_->file->file_name());
        return s;
      } else {
        // Emit a warning, but ignore the error status
        ROCKS_LOG_WARN(rep_->ioptions.logger,
                       "Failed to find the the UDI block %s in file %s; %s",
                       udi_name.c_str(), rep_->file->file_name().c_str(),
                       s.ToString().c_str());
        s = Status::OK();
      }
    }

    // If the UDI block size is 0, that means there's effectively no user
    // defined index. In that case, skip setting up the reader.
    if (udi_block_handle.size() > 0) {
      // Read the block, and allocate on heap or pin in cache. The UDI block is
      // not compressed. RetrieveBlock will verify the checksum.
      if (s.ok()) {
        s = RetrieveBlock(prefetch_buffer, ro, udi_block_handle,
                          rep_->decompressor.get(), &rep_->udi_block,
                          /*get_context=*/nullptr, lookup_context,
                          /*for_compaction=*/false, use_cache,
                          /*async_read=*/false,
                          /*use_block_cache_for_lookup=*/false);
      }
      if (s.ok()) {
        assert(!rep_->udi_block.IsEmpty());

        std::unique_ptr<UserDefinedIndexReader> udi_reader;
        UserDefinedIndexOption udi_option;
        udi_option.comparator = rep_->internal_comparator.user_comparator();
        s = table_options.user_defined_index_factory->NewReader(
            udi_option, rep_->udi_block.GetValue()->data, udi_reader);
        if (s.ok()) {
          if (udi_reader) {
            index_reader = std::make_unique<UserDefinedIndexReaderWrapper>(
                udi_name, std::move(index_reader), std::move(udi_reader));
          } else {
            s = Status::Corruption("Failed to create UDI reader for " +
                                   udi_name + " in file " +
                                   rep_->file->file_name());
          }
        }
      }
    }
  }

  rep_->index_reader = std::move(index_reader);

  // The partitions of partitioned index are always stored in cache. They
  // are hence follow the configuration for pin and prefetch regardless of
  // the value of cache_index_and_filter_blocks
  if (s.ok() && (prefetch_all || pin_partition)) {
    s = rep_->index_reader->CacheDependencies(ro, pin_partition,
                                              prefetch_buffer);
  }
  if (!s.ok()) {
    return s;
  }

  // pin the first level of filter
  const bool pin_filter =
      rep_->filter_type == Rep::FilterType::kPartitionedFilter
          ? pin_top_level_index
          : pin_unpartitioned;
  // prefetch the first level of filter
  // WART: this might be redundant (unnecessary cache hit) if !pin_filter,
  // depending on prepopulate_block_cache option
  const bool prefetch_filter = prefetch_all || pin_filter;

  if (rep_->filter_policy) {
    auto filter = new_table->CreateFilterBlockReader(
        ro, prefetch_buffer, use_cache, prefetch_filter, pin_filter,
        lookup_context);

    if (filter) {
      // Refer to the comment above about paritioned indexes always being cached
      if (prefetch_all || pin_partition) {
        s = filter->CacheDependencies(ro, pin_partition, prefetch_buffer);
        if (!s.ok()) {
          return s;
        }
      }
      rep_->filter = std::move(filter);
    }
  }

  // NOTE: before the fix to https://github.com/facebook/rocksdb/issues/12409, a
  // file could have a (de)compression dictionary block without a configured
  // compression, so we need to ignore the dictionary in that case.
  if (!rep_->compression_dict_handle.IsNull() && rep_->decompressor) {
    std::unique_ptr<UncompressionDictReader> uncompression_dict_reader;
    s = UncompressionDictReader::Create(
        this, ro, prefetch_buffer, use_cache, prefetch_all || pin_unpartitioned,
        pin_unpartitioned, lookup_context, &uncompression_dict_reader);
    if (!s.ok()) {
      return s;
    }

    rep_->uncompression_dict_reader = std::move(uncompression_dict_reader);
  }

  assert(s.ok());
  return s;
}

void BlockBasedTable::SetupForCompaction() {}

std::shared_ptr<const TableProperties> BlockBasedTable::GetTableProperties()
    const {
  return rep_->table_properties;
}

const SeqnoToTimeMapping& BlockBasedTable::GetSeqnoToTimeMapping() const {
  return rep_->seqno_to_time_mapping;
}

size_t BlockBasedTable::ApproximateMemoryUsage() const {
  size_t usage = 0;
  if (rep_) {
    usage += rep_->ApproximateMemoryUsage();
  } else {
    return usage;
  }
  if (rep_->filter) {
    usage += rep_->filter->ApproximateMemoryUsage();
  }
  if (rep_->index_reader) {
    usage += rep_->index_reader->ApproximateMemoryUsage();
  }
  if (rep_->uncompression_dict_reader) {
    usage += rep_->uncompression_dict_reader->ApproximateMemoryUsage();
  }
  if (rep_->table_properties) {
    usage += rep_->table_properties->ApproximateMemoryUsage();
  }
  return usage;
}

// Load the meta-index-block from the file. On success, return the loaded
// metaindex
// block and its iterator.
Status BlockBasedTable::ReadMetaIndexBlock(
    const ReadOptions& ro, FilePrefetchBuffer* prefetch_buffer,
    std::unique_ptr<Block>* metaindex_block,
    std::unique_ptr<InternalIterator>* iter) {
  // TODO(sanjay): Skip this if footer.metaindex_handle() size indicates
  // it is an empty block.
  std::unique_ptr<Block_kMetaIndex> metaindex;
  Status s = ReadAndParseBlockFromFile(
      rep_->file.get(), prefetch_buffer, rep_->footer, ro,
      rep_->footer.metaindex_handle(), &metaindex, rep_->ioptions,
      rep_->create_context, true /*maybe_compressed*/, rep_->decompressor.get(),
      rep_->persistent_cache_options, GetMemoryAllocator(rep_->table_options),
      false /* for_compaction */, false /* async_read */);

  if (!s.ok()) {
    ROCKS_LOG_ERROR(rep_->ioptions.logger,
                    "Encountered error while reading data from properties"
                    " block %s",
                    s.ToString().c_str());
    return s;
  }

  *metaindex_block = std::move(metaindex);
  // meta block uses bytewise comparator.
  iter->reset(metaindex_block->get()->NewMetaIterator());
  return Status::OK();
}

template <typename TBlocklike>
Cache::Priority BlockBasedTable::GetCachePriority() const {
  // Here we treat the legacy name "...index_and_filter_blocks..." to mean all
  // metadata blocks that might go into block cache, EXCEPT only those needed
  // for the read path (Get, etc.). TableProperties should not be needed on the
  // read path (prefix extractor setting is an O(1) size special case that we
  // are working not to require from TableProperties), so it is not given
  // high-priority treatment if it should go into BlockCache.
  if constexpr (TBlocklike::kBlockType == BlockType::kData ||
                TBlocklike::kBlockType == BlockType::kProperties) {
    return Cache::Priority::LOW;
  } else if (rep_->table_options
                 .cache_index_and_filter_blocks_with_high_priority) {
    return Cache::Priority::HIGH;
  } else {
    return Cache::Priority::LOW;
  }
}

template <typename TBlocklike>
WithBlocklikeCheck<Status, TBlocklike> BlockBasedTable::GetDataBlockFromCache(
    const Slice& cache_key, BlockCacheInterface<TBlocklike> block_cache,
    CachableEntry<TBlocklike>* out_parsed_block, GetContext* get_context,
    UnownedPtr<Decompressor> decomp) const {
  assert(out_parsed_block);
  assert(out_parsed_block->IsEmpty());

  Status s;
  Statistics* statistics = rep_->ioptions.statistics.get();

  // Lookup uncompressed cache first
  if (block_cache) {
    assert(!cache_key.empty());
    typename BlockCacheInterface<TBlocklike>::TypedHandle* cache_handle;
    if (decomp.get() != rep_->decompressor.get() && decomp) {
      // `decomp` must be a dictionary-aware decompressor, which is only
      // available in the block cache (so that dictionaries can be evicted
      // from memory) and can't live in the table reader.
      // NOTE: inefficient BlockCreateContext copy for dict-aware decompressor
      // (see TODO in block_cache.h)
      BlockCreateContext create_ctx = rep_->create_context;
      create_ctx.decompressor = decomp.get();
      cache_handle = block_cache.LookupFull(
          cache_key, &create_ctx, GetCachePriority<TBlocklike>(), statistics,
          rep_->ioptions.lowest_used_cache_tier);
    } else {
      cache_handle = block_cache.LookupFull(
          cache_key, &rep_->create_context, GetCachePriority<TBlocklike>(),
          statistics, rep_->ioptions.lowest_used_cache_tier);
    }

    // Avoid updating metrics here if the handle is not complete yet. This
    // happens with MultiGet and secondary cache. So update the metrics only
    // if its a miss, or a hit and value is ready
    if (!cache_handle) {
      UpdateCacheMissMetrics(TBlocklike::kBlockType, get_context);
    } else {
      TBlocklike* value = block_cache.Value(cache_handle);
      if (value) {
        UpdateCacheHitMetrics(TBlocklike::kBlockType, get_context,
                              block_cache.get()->GetUsage(cache_handle));
      }
      out_parsed_block->SetCachedValue(value, block_cache.get(), cache_handle);
      return s;
    }
  }

  // If not found, search from the compressed block cache.
  assert(out_parsed_block->IsEmpty());

  return s;
}

template <typename TBlocklike>
WithBlocklikeCheck<Status, TBlocklike> BlockBasedTable::PutDataBlockToCache(
    const Slice& cache_key, BlockCacheInterface<TBlocklike> block_cache,
    CachableEntry<TBlocklike>* out_parsed_block,
    BlockContents&& uncompressed_block_contents,
    BlockContents&& compressed_block_contents, CompressionType block_comp_type,
    UnownedPtr<Decompressor> decomp, MemoryAllocator* memory_allocator,
    GetContext* get_context) const {
  const ImmutableOptions& ioptions = rep_->ioptions;
  assert(out_parsed_block);
  assert(out_parsed_block->IsEmpty());

  Status s;
  Statistics* statistics = ioptions.stats;

  std::unique_ptr<TBlocklike> block_holder;
  if (block_comp_type != kNoCompression &&
      uncompressed_block_contents.data.empty()) {
    assert(compressed_block_contents.data.data());
    // Retrieve the uncompressed contents into a new buffer
    s = DecompressBlockData(
        compressed_block_contents.data.data(),
        compressed_block_contents.data.size(), block_comp_type, *decomp,
        &uncompressed_block_contents, ioptions, memory_allocator);
    if (!s.ok()) {
      return s;
    }
  }
  rep_->create_context.Create(&block_holder,
                              std::move(uncompressed_block_contents));

  // insert into uncompressed block cache
  if (block_cache && block_holder->own_bytes()) {
    size_t charge = block_holder->ApproximateMemoryUsage();
    BlockCacheTypedHandle<TBlocklike>* cache_handle = nullptr;
    s = block_cache.InsertFull(cache_key, block_holder.get(), charge,
                               &cache_handle, GetCachePriority<TBlocklike>(),
                               rep_->ioptions.lowest_used_cache_tier,
                               compressed_block_contents.data, block_comp_type);

    if (s.ok()) {
      assert(cache_handle != nullptr);
      out_parsed_block->SetCachedValue(block_holder.release(),
                                       block_cache.get(), cache_handle);

      UpdateCacheInsertionMetrics(TBlocklike::kBlockType, get_context, charge,
                                  s.IsOkOverwritten(), rep_->ioptions.stats);
    } else {
      RecordTick(statistics, BLOCK_CACHE_ADD_FAILURES);
    }
  } else {
    out_parsed_block->SetOwnedValue(std::move(block_holder));
  }

  return s;
}

std::unique_ptr<FilterBlockReader> BlockBasedTable::CreateFilterBlockReader(
    const ReadOptions& ro, FilePrefetchBuffer* prefetch_buffer, bool use_cache,
    bool prefetch, bool pin, BlockCacheLookupContext* lookup_context) {
  auto& rep = rep_;
  auto filter_type = rep->filter_type;
  if (filter_type == Rep::FilterType::kNoFilter) {
    return std::unique_ptr<FilterBlockReader>();
  }

  assert(rep->filter_policy);

  switch (filter_type) {
    case Rep::FilterType::kPartitionedFilter:
      return PartitionedFilterBlockReader::Create(
          this, ro, prefetch_buffer, use_cache, prefetch, pin, lookup_context);

    case Rep::FilterType::kFullFilter:
      return FullFilterBlockReader::Create(this, ro, prefetch_buffer, use_cache,
                                           prefetch, pin, lookup_context);

    default:
      // filter_type is either kNoFilter (exited the function at the first if),
      // or it must be covered in this switch block
      assert(false);
      return std::unique_ptr<FilterBlockReader>();
  }
}

// disable_prefix_seek should be set to true when prefix_extractor found in SST
// differs from the one in mutable_cf_options and index type is HashBasedIndex
InternalIteratorBase<IndexValue>* BlockBasedTable::NewIndexIterator(
    const ReadOptions& read_options, bool disable_prefix_seek,
    IndexBlockIter* input_iter, GetContext* get_context,
    BlockCacheLookupContext* lookup_context) const {
  assert(rep_ != nullptr);
  assert(rep_->index_reader != nullptr);

  // We don't return pinned data from index blocks, so no need
  // to set `block_contents_pinned`.
  return rep_->index_reader->NewIterator(read_options, disable_prefix_seek,
                                         input_iter, get_context,
                                         lookup_context);
}

// TODO?
template <>
DataBlockIter* BlockBasedTable::InitBlockIterator<DataBlockIter>(
    const Rep* rep, Block* block, BlockType block_type,
    DataBlockIter* input_iter, bool block_contents_pinned) {
  return block->NewDataIterator(rep->internal_comparator.user_comparator(),
                                rep->get_global_seqno(block_type), input_iter,
                                rep->ioptions.stats, block_contents_pinned,
                                rep->user_defined_timestamps_persisted);
}

// TODO?
template <>
IndexBlockIter* BlockBasedTable::InitBlockIterator<IndexBlockIter>(
    const Rep* rep, Block* block, BlockType block_type,
    IndexBlockIter* input_iter, bool block_contents_pinned) {
  return block->NewIndexIterator(
      rep->internal_comparator.user_comparator(),
      rep->get_global_seqno(block_type), input_iter, rep->ioptions.stats,
      /* total_order_seek */ true, rep->index_has_first_key,
      rep->index_key_includes_seq, rep->index_value_is_full,
      block_contents_pinned, rep->user_defined_timestamps_persisted);
}

DataBlockIter* BlockBasedTable::KVSepBptreeNewLeafBlockIterator(
    const ReadOptions& ro, const BlockHandle& leaf_handle,
    DataBlockIter* input_iter, GetContext* get_context,
    BlockCacheLookupContext* lookup_context,
    FilePrefetchBuffer* prefetch_buffer, bool for_compaction, bool async_read,
    Status& s, bool use_block_cache_for_lookup) const {
  PERF_TIMER_GUARD(new_table_block_iter_nanos);

  DataBlockIter* iter = input_iter != nullptr ? input_iter : new DataBlockIter;
  if (!s.ok()) {
    iter->Invalidate(s);
    return iter;
  }

  CachableEntry<Block> block;
  s = RetrieveBlock<Block_kIndex>(
      prefetch_buffer, ro, leaf_handle, rep_->decompressor.get(),
      &block.As<Block_kIndex>(), get_context, lookup_context, for_compaction,
      /*use_cache=*/true, async_read, use_block_cache_for_lookup);

  if (s.IsTryAgain() && async_read) {
    return iter;
  }
  if (!s.ok()) {
    assert(block.IsEmpty());
    iter->Invalidate(s);
    return iter;
  }
  assert(block.GetValue() != nullptr);

  const bool block_contents_pinned =
      block.IsCached() ||
      (!block.GetValue()->own_bytes() && rep_->immortal_table);
  iter = InitBlockIterator<DataBlockIter>(rep_, block.GetValue(),
                                         BlockType::kIndex, iter,
                                         block_contents_pinned);
  if (block.IsCached()) {
    iter->SetCacheHandle(block.GetCacheHandle());
  }
  block.TransferTo(iter);
  return iter;
}

// Right now only called for Data blocks.
template <typename TBlocklike>
Status BlockBasedTable::LookupAndPinBlocksInCache(
    const ReadOptions& ro, const BlockHandle& handle,
    CachableEntry<TBlocklike>* out_parsed_block) const {
  BlockCacheInterface<TBlocklike> block_cache{
      rep_->table_options.block_cache.get()};

  assert(block_cache);

  Status s;
  CachableEntry<DecompressorDict> cached_dict;
  if (rep_->uncompression_dict_reader) {
    s = rep_->uncompression_dict_reader->GetOrReadUncompressionDictionary(
        /* prefetch_buffer= */ nullptr, ro,
        /* get_context= */ nullptr, /* lookup_context= */ nullptr,
        &cached_dict);
    if (!s.ok()) {
      return s;
    }
    if (!cached_dict.GetValue()) {
      return Status::Corruption("Success but no dictionary read");
    }
  }

  // Do the lookup.
  CacheKey key_data = GetCacheKey(rep_->base_cache_key, handle);
  const Slice key = key_data.AsSlice();

  Statistics* statistics = rep_->ioptions.statistics.get();

  typename BlockCacheInterface<TBlocklike>::TypedHandle* cache_handle;
  if (cached_dict.GetValue()) {
    // NOTE: inefficient BlockCreateContext copy for dict-aware decompressor
    // (see TODO in block_cache.h)
    BlockCreateContext create_ctx = rep_->create_context;
    create_ctx.decompressor = cached_dict.GetValue()->decompressor_.get();
    cache_handle = block_cache.LookupFull(
        key, &create_ctx, GetCachePriority<TBlocklike>(), statistics,
        rep_->ioptions.lowest_used_cache_tier);
  } else {
    cache_handle = block_cache.LookupFull(
        key, &rep_->create_context, GetCachePriority<TBlocklike>(), statistics,
        rep_->ioptions.lowest_used_cache_tier);
  }

  if (!cache_handle) {
    UpdateCacheMissMetrics(TBlocklike::kBlockType, /* get_context = */ nullptr);
    return s;
  }

  // Found in Cache.
  TBlocklike* value = block_cache.Value(cache_handle);
  if (value) {
    UpdateCacheHitMetrics(TBlocklike::kBlockType, /* get_context = */ nullptr,
                          block_cache.get()->GetUsage(cache_handle));
  }
  out_parsed_block->SetCachedValue(value, block_cache.get(), cache_handle);

  assert(!out_parsed_block->IsEmpty());

  return s;
}

template <typename TBlocklike>
Status BlockBasedTable::CreateAndPinBlockInCache(
    const ReadOptions& ro, const BlockHandle& handle,
    UnownedPtr<Decompressor> decomp, BlockContents* contents,
    CachableEntry<TBlocklike>* out_parsed_block) const {
  CompressionType compression_type = GetBlockCompressionType(*contents);
  // If we don't own the contents and we don't need to decompress, copy
  // the block to heap in order to have ownership. If decompression is
  // needed, then the decompressor will allocate a buffer.
  if (!contents->own_bytes() && compression_type == kNoCompression) {
    Slice src = Slice(contents->data.data(), BlockSizeWithTrailer(handle));
    *contents = BlockContents(
        CopyBufferToHeap(GetMemoryAllocator(rep_->table_options), src),
        handle.size());
#ifndef NDEBUG
    contents->has_trailer = true;
#endif
  }

  Status s;
  if (ro.fill_cache) {
    s = MaybeReadBlockAndLoadToCache(nullptr, ro, handle, decomp,
                                     /*for_compaction=*/false, out_parsed_block,
                                     nullptr, nullptr, contents,
                                     /*async_read=*/false,
                                     /*use_block_cache_for_lookup=*/true);
  }

  if (!s.ok()) {
    return s;
  }

  // fill_cache could be false, or no block cache is configured. In that
  // case, decompress if necessary and take ownership of the block
  if (out_parsed_block->GetValue() == nullptr && contents != nullptr) {
    BlockContents tmp_contents;
    if (compression_type != kNoCompression) {
      s = DecompressSerializedBlock(contents->data.data(), handle.size(),
                                    compression_type, *decomp, &tmp_contents,
                                    rep_->ioptions,
                                    GetMemoryAllocator(rep_->table_options));
    } else {
      tmp_contents = std::move(*contents);
    }
    if (s.ok()) {
      std::unique_ptr<TBlocklike> block_holder;
      rep_->create_context.Create(&block_holder, std::move(tmp_contents));
      out_parsed_block->SetOwnedValue(std::move(block_holder));
    }
  }
  return s;
}

// If contents is nullptr, this function looks up the block caches for the
// data block referenced by handle, and read the block from disk if necessary.
// If contents is non-null, it skips the cache lookup and disk read, since
// the caller has already read it. In both cases, if ro.fill_cache is true,
// it inserts the block into the block cache.
template <typename TBlocklike>
WithBlocklikeCheck<Status, TBlocklike>
BlockBasedTable::MaybeReadBlockAndLoadToCache(
    FilePrefetchBuffer* prefetch_buffer, const ReadOptions& ro,
    const BlockHandle& handle, UnownedPtr<Decompressor> decomp,
    bool for_compaction, CachableEntry<TBlocklike>* out_parsed_block,
    GetContext* get_context, BlockCacheLookupContext* lookup_context,
    BlockContents* contents, bool async_read,
    bool use_block_cache_for_lookup) const {
  assert(out_parsed_block != nullptr);
  const bool no_io = (ro.read_tier == kBlockCacheTier);
  BlockCacheInterface<TBlocklike> block_cache{
      rep_->table_options.block_cache.get()};
  // First, try to get the block from the cache
  //
  // If either block cache is enabled, we'll try to read from it.
  Status s;
  CacheKey key_data;
  Slice key;
  bool is_cache_hit = false;
  if (block_cache) {
    // create key for block cache
    key_data = GetCacheKey(rep_->base_cache_key, handle);
    key = key_data.AsSlice();

    if (!contents) {
      if (use_block_cache_for_lookup) {
        s = GetDataBlockFromCache(key, block_cache, out_parsed_block,
                                  get_context, decomp);
        // Value could still be null at this point, so check the cache handle
        // and update the read pattern for prefetching
        if (out_parsed_block->GetValue() ||
            out_parsed_block->GetCacheHandle()) {
          // TODO(haoyu): Differentiate cache hit on uncompressed block cache
          // and compressed block cache.
          is_cache_hit = true;
          RecordExperimentalKVSepBlockCacheHit<TBlocklike>(rep_->ioptions.stats);
          if (prefetch_buffer) {
            // Update the block details so that PrefetchBuffer can use the read
            // pattern to determine if reads are sequential or not for
            // prefetching. It should also take in account blocks read from
            // cache.
            prefetch_buffer->UpdateReadPattern(
                handle.offset(), BlockSizeWithTrailer(handle),
                ro.adaptive_readahead /*decrease_readahead_size*/);
          }
        } else {
          RecordExperimentalKVSepBlockCacheMiss<TBlocklike>(rep_->ioptions.stats);
        }
      }
    }

    // Can't find the block from the cache. If I/O is allowed, read from the
    // file.
    if (out_parsed_block->GetValue() == nullptr &&
        out_parsed_block->GetCacheHandle() == nullptr && !no_io &&
        ro.fill_cache) {
      Statistics* statistics = rep_->ioptions.stats;
      const bool maybe_compressed =
          BlockTypeMaybeCompressed(TBlocklike::kBlockType) &&
          rep_->decompressor;
      // This flag, if true, tells BlockFetcher to return the uncompressed
      // block when ReadBlockContents() is called.
      const bool do_uncompress = maybe_compressed;
      CompressionType contents_comp_type;
      // Maybe serialized or uncompressed
      BlockContents tmp_contents;
      BlockContents uncomp_contents;
      BlockContents comp_contents;
      if (!contents) {
        Histograms histogram = for_compaction ? READ_BLOCK_COMPACTION_MICROS
                                              : READ_BLOCK_GET_MICROS;
        StopWatch sw(rep_->ioptions.clock, statistics, histogram);
        // Setting do_uncompress to false may cause an extra mempcy in the
        // following cases -
        // 1. Compression is enabled, but block is not actually compressed
        // 2. Compressed block is in the prefetch buffer
        // 3. Direct IO
        //
        // It would also cause a memory allocation to be used rather than
        // stack if the compressed block size is < 5KB
        BlockFetcher block_fetcher(
            rep_->file.get(), prefetch_buffer, rep_->footer, ro, handle,
            &tmp_contents, rep_->table_options.super_block_alignment_size,
            rep_->table_options.enable_super_block_read_coalescing,
            rep_->ioptions, do_uncompress, maybe_compressed, TBlocklike::kBlockType,
            decomp, rep_->persistent_cache_options,
            GetMemoryAllocator(rep_->table_options),
            /*allocator=*/nullptr);

        // If prefetch_buffer is not allocated, it will fallback to synchronous
        // reading of block contents.
        if (async_read && prefetch_buffer != nullptr) {
          s = block_fetcher.ReadAsyncBlockContents();
          if (!s.ok()) {
            return s;
          }
        } else {
          s = block_fetcher.ReadBlockContents();
        }

        contents_comp_type = block_fetcher.compression_type();
        if (get_context) {
          switch (TBlocklike::kBlockType) {
            case BlockType::kIndex:
              ++get_context->get_context_stats_.num_index_read;
              break;
            case BlockType::kFilter:
            case BlockType::kFilterPartitionIndex:
              ++get_context->get_context_stats_.num_filter_read;
              break;
            default:
              break;
          }
        }
        if (s.ok()) {
          RecordExperimentalKVSepBlockFileRead<TBlocklike>(statistics, handle);
        }
        if (s.ok()) {
          if (do_uncompress && contents_comp_type != kNoCompression) {
            comp_contents = BlockContents(block_fetcher.GetCompressedBlock());
            uncomp_contents = std::move(tmp_contents);
          } else if (contents_comp_type != kNoCompression) {
            // do_uncompress must be false, so output of BlockFetcher is
            // compressed
            comp_contents = std::move(tmp_contents);
          } else {
            uncomp_contents = std::move(tmp_contents);
          }

          // If filling cache is allowed and a cache is configured, try to put
          // the block to the cache. Do this here while block_fetcher is in
          // scope, since comp_contents will be a reference to the compressed
          // block in block_fetcher
          s = PutDataBlockToCache(
              key, block_cache, out_parsed_block, std::move(uncomp_contents),
              std::move(comp_contents), contents_comp_type, decomp,
              GetMemoryAllocator(rep_->table_options), get_context);
        }
      } else {
        contents_comp_type = GetBlockCompressionType(*contents);
        if (contents_comp_type != kNoCompression) {
          comp_contents = std::move(*contents);
        } else {
          uncomp_contents = std::move(*contents);
        }

        if (s.ok()) {
          // If filling cache is allowed and a cache is configured, try to put
          // the block to the cache.
          s = PutDataBlockToCache(
              key, block_cache, out_parsed_block, std::move(uncomp_contents),
              std::move(comp_contents), contents_comp_type, decomp,
              GetMemoryAllocator(rep_->table_options), get_context);
        }
      }
    }
  }

  // TODO: optimize so that lookup_context != nullptr implies the others
  if (block_cache_tracer_ && block_cache_tracer_->is_tracing_enabled() &&
      lookup_context) {
    SaveLookupContextOrTraceRecord(
        key, is_cache_hit, ro, out_parsed_block->GetValue(), lookup_context);
  }

  assert(s.ok() || out_parsed_block->GetValue() == nullptr);
  return s;
}

template <typename TBlocklike>
WithBlocklikeCheck<void, TBlocklike>
BlockBasedTable::SaveLookupContextOrTraceRecord(
    const Slice& block_key, bool is_cache_hit, const ReadOptions& ro,
    const TBlocklike* parsed_block_value,
    BlockCacheLookupContext* lookup_context) const {
  assert(lookup_context);
  size_t usage = 0;
  uint64_t nkeys = 0;
  if (parsed_block_value) {
    // Approximate the number of keys in the block using restarts.
    int interval = rep_->table_options.block_restart_interval;
    nkeys = interval * GetBlockNumRestarts(*parsed_block_value);
    // On average, the last restart should be just over half utilized.
    // Specifically, 1..N should be N/2 + 0.5. For example, 7 -> 4, 8 -> 4.5.
    // Use the get_id to alternate between rounding up vs. down.
    if (nkeys > 0) {
      bool rounding = static_cast<int>(lookup_context->get_id) & 1;
      nkeys -= (interval - rounding) / 2;
    }
    usage = parsed_block_value->ApproximateMemoryUsage();
  }
  TraceType trace_block_type = TraceType::kTraceMax;
  switch (TBlocklike::kBlockType) {
    case BlockType::kData:
      trace_block_type = TraceType::kBlockTraceDataBlock;
      break;
    case BlockType::kFilter:
    case BlockType::kFilterPartitionIndex:
      trace_block_type = TraceType::kBlockTraceFilterBlock;
      break;
    case BlockType::kCompressionDictionary:
      trace_block_type = TraceType::kBlockTraceUncompressionDictBlock;
      break;
    case BlockType::kRangeDeletion:
      trace_block_type = TraceType::kBlockTraceRangeDeletionBlock;
      break;
    case BlockType::kIndex:
    case BlockType::kUserDefinedIndex:
      trace_block_type = TraceType::kBlockTraceIndexBlock;
      break;
    default:
      // This cannot happen.
      assert(false);
      break;
  }
  const bool no_io = ro.read_tier == kBlockCacheTier;
  bool no_insert = no_io || !ro.fill_cache;
  if (BlockCacheTraceHelper::IsGetOrMultiGetOnDataBlock(
          trace_block_type, lookup_context->caller)) {
    // Make a copy of the block key here since it will be logged later.
    lookup_context->FillLookupContext(is_cache_hit, no_insert, trace_block_type,
                                      /*block_size=*/usage,
                                      block_key.ToString(), nkeys);

    // Defer logging the access to Get() and MultiGet() to trace additional
    // information, e.g., referenced_key
  } else {
    // Avoid making copy of block_key if it doesn't need to be saved in
    // BlockCacheLookupContext
    lookup_context->FillLookupContext(is_cache_hit, no_insert, trace_block_type,
                                      /*block_size=*/usage,
                                      /*block_key=*/{}, nkeys);

    // Fill in default values for irrelevant/unknown fields
    FinishTraceRecord(*lookup_context, block_key,
                      lookup_context->referenced_key,
                      /*does_referenced_key_exist*/ false,
                      /*referenced_data_size*/ 0);
  }
}

void BlockBasedTable::FinishTraceRecord(
    const BlockCacheLookupContext& lookup_context, const Slice& block_key,
    const Slice& referenced_key, bool does_referenced_key_exist,
    uint64_t referenced_data_size) const {
  // Avoid making copy of referenced_key if it doesn't need to be saved in
  // BlockCacheLookupContext
  BlockCacheTraceRecord access_record(
      rep_->ioptions.clock->NowMicros(),
      /*block_key=*/"", lookup_context.block_type, lookup_context.block_size,
      rep_->cf_id_for_tracing(),
      /*cf_name=*/"", rep_->level_for_tracing(), rep_->sst_number_for_tracing(),
      lookup_context.caller, lookup_context.is_cache_hit,
      lookup_context.no_insert, lookup_context.get_id,
      lookup_context.get_from_user_specified_snapshot,
      /*referenced_key=*/"", referenced_data_size,
      lookup_context.num_keys_in_block, does_referenced_key_exist);
  // TODO: Should handle status here?
  block_cache_tracer_
      ->WriteBlockAccess(access_record, block_key, rep_->cf_name_for_tracing(),
                         referenced_key)
      .PermitUncheckedError();
}

template <typename TBlocklike /*, auto*/>
WithBlocklikeCheck<Status, TBlocklike> BlockBasedTable::RetrieveBlock(
    FilePrefetchBuffer* prefetch_buffer, const ReadOptions& ro,
    const BlockHandle& handle, UnownedPtr<Decompressor> decomp,
    CachableEntry<TBlocklike>* out_parsed_block, GetContext* get_context,
    BlockCacheLookupContext* lookup_context, bool for_compaction,
    bool use_cache, bool async_read, bool use_block_cache_for_lookup) const {
  assert(out_parsed_block);
  assert(out_parsed_block->IsEmpty());

  Status s;
  if (use_cache) {
    s = MaybeReadBlockAndLoadToCache(
        prefetch_buffer, ro, handle, decomp, for_compaction, out_parsed_block,
        get_context, lookup_context,
        /*contents=*/nullptr, async_read, use_block_cache_for_lookup);

    if (!s.ok()) {
      return s;
    }

    if (out_parsed_block->GetValue() != nullptr ||
        out_parsed_block->GetCacheHandle() != nullptr) {
      assert(s.ok());
      return s;
    }
  }

  assert(out_parsed_block->IsEmpty());

  const bool no_io = ro.read_tier == kBlockCacheTier;
  if (no_io) {
    return Status::Incomplete("no blocking io");
  }

  const bool maybe_compressed =
      BlockTypeMaybeCompressed(TBlocklike::kBlockType) && rep_->decompressor;
  std::unique_ptr<TBlocklike> block;

  {
    Histograms histogram =
        for_compaction ? READ_BLOCK_COMPACTION_MICROS : READ_BLOCK_GET_MICROS;
    StopWatch sw(rep_->ioptions.clock, rep_->ioptions.stats, histogram);
    s = ReadAndParseBlockFromFile(
        rep_->file.get(), prefetch_buffer, rep_->footer, ro, handle, &block,
        rep_->ioptions, rep_->create_context, maybe_compressed, decomp,
        rep_->persistent_cache_options, GetMemoryAllocator(rep_->table_options),
        for_compaction, async_read);

    if (get_context) {
      switch (TBlocklike::kBlockType) {
        case BlockType::kIndex:
          ++(get_context->get_context_stats_.num_index_read);
          break;
        case BlockType::kFilter:
        case BlockType::kFilterPartitionIndex:
          ++(get_context->get_context_stats_.num_filter_read);
          break;
        default:
          break;
      }
    }
  }

  if (!s.ok()) {
    return s;
  }
  RecordExperimentalKVSepBlockFileRead<TBlocklike>(rep_->ioptions.stats, handle);

  out_parsed_block->SetOwnedValue(std::move(block));

  assert(s.ok());
  return s;
}

BlockBasedTable::PartitionedIndexIteratorState::PartitionedIndexIteratorState(
    const BlockBasedTable* table,
    UnorderedMap<uint64_t, CachableEntry<Block>>* block_map)
    : table_(table), block_map_(block_map) {}

InternalIteratorBase<IndexValue>*
BlockBasedTable::PartitionedIndexIteratorState::NewSecondaryIterator(
    const BlockHandle& handle) {
  // Return a block iterator on the index partition
  auto block = block_map_->find(handle.offset());
  // block_map_ must be exhaustive
  if (block == block_map_->end()) {
    assert(false);
    // Signal problem to caller
    return nullptr;
  }
  const Rep* rep = table_->get_rep();
  assert(rep);

  Statistics* kNullStats = nullptr;
  // We don't return pinned data from index blocks, so no need
  // to set `block_contents_pinned`.
  return block->second.GetValue()->NewIndexIterator(
      rep->internal_comparator.user_comparator(),
      rep->get_global_seqno(BlockType::kIndex), nullptr, kNullStats, true,
      rep->index_has_first_key, rep->index_key_includes_seq,
      rep->index_value_is_full, /*block_contents_pinned=*/false,
      rep->user_defined_timestamps_persisted);
}

// This will be broken if the user specifies an unusual implementation
// of Options.comparator, or if the user specifies an unusual
// definition of prefixes in BlockBasedTableOptions.filter_policy.
// In particular, we require the following three properties:
//
// 1) key.starts_with(prefix(key))
// 2) Compare(prefix(key), key) <= 0.
// 3) If Compare(key1, key2) <= 0, then Compare(prefix(key1), prefix(key2)) <= 0
//
// If read_options.read_tier == kBlockCacheTier, this method will do no I/O and
// will return true if the filter block is not in memory and not found in block
// cache.
//
// REQUIRES: this method shouldn't be called while the DB lock is held.
bool BlockBasedTable::PrefixRangeMayMatch(
    const Slice& internal_key, const ReadOptions& read_options,
    const SliceTransform* options_prefix_extractor,
    const bool need_upper_bound_check, BlockCacheLookupContext* lookup_context,
    bool* filter_checked) const {
  if (!rep_->filter_policy) {
    return true;
  }

  const SliceTransform* prefix_extractor;

  if (rep_->table_prefix_extractor == nullptr) {
    if (need_upper_bound_check) {
      return true;
    }
    prefix_extractor = options_prefix_extractor;
  } else {
    prefix_extractor = rep_->table_prefix_extractor.get();
  }
  auto ts_sz = rep_->internal_comparator.user_comparator()->timestamp_size();
  auto user_key_without_ts =
      ExtractUserKeyAndStripTimestamp(internal_key, ts_sz);
  if (!prefix_extractor->InDomain(user_key_without_ts)) {
    return true;
  }

  bool may_match = true;

  FilterBlockReader* const filter = rep_->filter.get();
  *filter_checked = false;
  if (filter != nullptr) {
    const Slice* const const_ikey_ptr = &internal_key;
    may_match = filter->RangeMayExist(
        read_options.iterate_upper_bound, user_key_without_ts, prefix_extractor,
        rep_->internal_comparator.user_comparator(), const_ikey_ptr,
        filter_checked, need_upper_bound_check, lookup_context, read_options);
  }

  return may_match;
}

bool BlockBasedTable::PrefixExtractorChanged(
    const SliceTransform* prefix_extractor) const {
  if (prefix_extractor == nullptr) {
    return true;
  } else if (prefix_extractor == rep_->table_prefix_extractor.get()) {
    return false;
  } else {
    return PrefixExtractorChangedHelper(rep_->table_properties.get(),
                                        prefix_extractor);
  }
}

Statistics* BlockBasedTable::GetStatistics() const {
  return rep_->ioptions.stats;
}
bool BlockBasedTable::IsLastLevel() const {
  return rep_->level == rep_->ioptions.num_levels - 1;
}

InternalIterator* BlockBasedTable::NewIterator(
    const ReadOptions& read_options, const SliceTransform* prefix_extractor,
    Arena* arena, bool skip_filters, TableReaderCaller caller,
    size_t compaction_readahead_size, bool allow_unprepared_value) {
  BlockCacheLookupContext lookup_context{caller};
  bool need_upper_bound_check =
      read_options.auto_prefix_mode || PrefixExtractorChanged(prefix_extractor);
  std::unique_ptr<InternalIteratorBase<IndexValue>> index_iter(NewIndexIterator(
      read_options,
      /*disable_prefix_seek=*/need_upper_bound_check &&
          rep_->index_type == BlockBasedTableOptions::kHashSearch,
      /*input_iter=*/nullptr, /*get_context=*/nullptr, &lookup_context));
  if (rep_->experimental_kvsep_bptree_enabled) {
    if (rep_->kvsep_bptree_leaf_format_version == 2) {
      if (arena == nullptr) {
        return new KVSepBptreeLeafV2TableIterator(
            this, read_options, rep_->internal_comparator, std::move(index_iter),
            caller);
      } else {
        auto* mem =
            arena->AllocateAligned(sizeof(KVSepBptreeLeafV2TableIterator));
        return new (mem) KVSepBptreeLeafV2TableIterator(
            this, read_options, rep_->internal_comparator, std::move(index_iter),
            caller);
      }
    }
    if (rep_->kvsep_bptree_leaf_format_version == 3) {
      if (arena == nullptr) {
        return new KVSepBptreePairV3TableIterator(
            this, read_options, rep_->internal_comparator, std::move(index_iter),
            caller);
      } else {
        auto* mem =
            arena->AllocateAligned(sizeof(KVSepBptreePairV3TableIterator));
        return new (mem) KVSepBptreePairV3TableIterator(
            this, read_options, rep_->internal_comparator, std::move(index_iter),
            caller);
      }
    }
  }
  if (arena == nullptr) {
    return new BlockBasedTableIterator(
        this, read_options, rep_->internal_comparator, std::move(index_iter),
        !skip_filters &&
            (!read_options.total_order_seek || read_options.auto_prefix_mode ||
             read_options.prefix_same_as_start) &&
            prefix_extractor != nullptr,
        need_upper_bound_check, prefix_extractor, caller,
        compaction_readahead_size, allow_unprepared_value);
  } else {
    auto* mem = arena->AllocateAligned(sizeof(BlockBasedTableIterator));
    return new (mem) BlockBasedTableIterator(
        this, read_options, rep_->internal_comparator, std::move(index_iter),
        !skip_filters &&
            (!read_options.total_order_seek || read_options.auto_prefix_mode ||
             read_options.prefix_same_as_start) &&
            prefix_extractor != nullptr,
        need_upper_bound_check, prefix_extractor, caller,
        compaction_readahead_size, allow_unprepared_value);
  }
}

FragmentedRangeTombstoneIterator* BlockBasedTable::NewRangeTombstoneIterator(
    const ReadOptions& read_options) {
  if (rep_->fragmented_range_dels == nullptr) {
    return nullptr;
  }
  SequenceNumber snapshot = kMaxSequenceNumber;
  if (read_options.snapshot != nullptr) {
    snapshot = read_options.snapshot->GetSequenceNumber();
  }
  return new FragmentedRangeTombstoneIterator(rep_->fragmented_range_dels,
                                              rep_->internal_comparator,
                                              snapshot, read_options.timestamp);
}

FragmentedRangeTombstoneIterator* BlockBasedTable::NewRangeTombstoneIterator(
    SequenceNumber read_seqno, const Slice* timestamp) {
  if (rep_->fragmented_range_dels == nullptr) {
    return nullptr;
  }
  return new FragmentedRangeTombstoneIterator(rep_->fragmented_range_dels,
                                              rep_->internal_comparator,
                                              read_seqno, timestamp);
}

bool BlockBasedTable::FullFilterKeyMayMatch(
    FilterBlockReader* filter, const Slice& internal_key,
    const SliceTransform* prefix_extractor, GetContext* get_context,
    BlockCacheLookupContext* lookup_context,
    const ReadOptions& read_options) const {
  if (filter == nullptr) {
    return true;
  }
  Slice user_key = ExtractUserKey(internal_key);
  const Slice* const const_ikey_ptr = &internal_key;
  bool may_match = true;
  size_t ts_sz = rep_->internal_comparator.user_comparator()->timestamp_size();
  Slice user_key_without_ts = StripTimestampFromUserKey(user_key, ts_sz);
  if (rep_->whole_key_filtering) {
    may_match = filter->KeyMayMatch(user_key_without_ts, const_ikey_ptr,
                                    get_context, lookup_context, read_options);
    if (may_match) {
      RecordTick(rep_->ioptions.stats, BLOOM_FILTER_FULL_POSITIVE);
      PERF_COUNTER_BY_LEVEL_ADD(bloom_filter_full_positive, 1, rep_->level);
    } else {
      RecordTick(rep_->ioptions.stats, BLOOM_FILTER_USEFUL);
      PERF_COUNTER_BY_LEVEL_ADD(bloom_filter_useful, 1, rep_->level);
    }
  } else if (!PrefixExtractorChanged(prefix_extractor) &&
             prefix_extractor->InDomain(user_key_without_ts)) {
    // FIXME ^^^: there should be no reason for Get() to depend on current
    // prefix_extractor at all. It should always use table_prefix_extractor.
    may_match = filter->PrefixMayMatch(
        prefix_extractor->Transform(user_key_without_ts), const_ikey_ptr,
        get_context, lookup_context, read_options);
    RecordTick(rep_->ioptions.stats, BLOOM_FILTER_PREFIX_CHECKED);
    if (may_match) {
      // Includes prefix stats
      PERF_COUNTER_BY_LEVEL_ADD(bloom_filter_full_positive, 1, rep_->level);
    } else {
      RecordTick(rep_->ioptions.stats, BLOOM_FILTER_PREFIX_USEFUL);
      // Includes prefix stats
      PERF_COUNTER_BY_LEVEL_ADD(bloom_filter_useful, 1, rep_->level);
    }
  }
  return may_match;
}

void BlockBasedTable::FullFilterKeysMayMatch(
    FilterBlockReader* filter, MultiGetRange* range,
    const SliceTransform* prefix_extractor,
    BlockCacheLookupContext* lookup_context,
    const ReadOptions& read_options) const {
  if (filter == nullptr) {
    return;
  }
  uint64_t before_keys = range->KeysLeft();
  assert(before_keys > 0);  // Caller should ensure
  if (rep_->whole_key_filtering) {
    filter->KeysMayMatch(range, lookup_context, read_options);
    uint64_t after_keys = range->KeysLeft();
    if (after_keys) {
      RecordTick(rep_->ioptions.stats, BLOOM_FILTER_FULL_POSITIVE, after_keys);
      PERF_COUNTER_BY_LEVEL_ADD(bloom_filter_full_positive, after_keys,
                                rep_->level);
    }
    uint64_t filtered_keys = before_keys - after_keys;
    if (filtered_keys) {
      RecordTick(rep_->ioptions.stats, BLOOM_FILTER_USEFUL, filtered_keys);
      PERF_COUNTER_BY_LEVEL_ADD(bloom_filter_useful, filtered_keys,
                                rep_->level);
    }
  } else if (!PrefixExtractorChanged(prefix_extractor)) {
    // FIXME ^^^: there should be no reason for MultiGet() to depend on current
    // prefix_extractor at all. It should always use table_prefix_extractor.
    filter->PrefixesMayMatch(range, prefix_extractor, lookup_context,
                             read_options);
    RecordTick(rep_->ioptions.stats, BLOOM_FILTER_PREFIX_CHECKED, before_keys);
    uint64_t after_keys = range->KeysLeft();
    if (after_keys) {
      // Includes prefix stats
      PERF_COUNTER_BY_LEVEL_ADD(bloom_filter_full_positive, after_keys,
                                rep_->level);
    }
    uint64_t filtered_keys = before_keys - after_keys;
    if (filtered_keys) {
      RecordTick(rep_->ioptions.stats, BLOOM_FILTER_PREFIX_USEFUL,
                 filtered_keys);
      // Includes prefix stats
      PERF_COUNTER_BY_LEVEL_ADD(bloom_filter_useful, filtered_keys,
                                rep_->level);
    }
  }
}

Status BlockBasedTable::ApproximateKeyAnchors(const ReadOptions& read_options,
                                              std::vector<Anchor>& anchors) {
  // We iterator the whole index block here. More efficient implementation
  // is possible if we push this operation into IndexReader. For example, we
  // can directly sample from restart block entries in the index block and
  // only read keys needed. Here we take a simple solution. Performance is
  // likely not to be a problem. We are compacting the whole file, so all
  // keys will be read out anyway. An extra read to index block might be
  // a small share of the overhead. We can try to optimize if needed.
  //
  // `CacheDependencies()` brings all the blocks into cache using one I/O. That
  // way the full index scan usually finds the index data it is looking for in
  // cache rather than doing an I/O for each "dependency" (partition).
  Status s = rep_->index_reader->CacheDependencies(
      read_options, false /* pin */, nullptr /* prefetch_buffer */);
  if (!s.ok()) {
    return s;
  }

  IndexBlockIter iiter_on_stack;
  auto iiter = NewIndexIterator(
      read_options, /*disable_prefix_seek=*/false, &iiter_on_stack,
      /*get_context=*/nullptr, /*lookup_context=*/nullptr);
  std::unique_ptr<InternalIteratorBase<IndexValue>> iiter_unique_ptr;
  if (iiter != &iiter_on_stack) {
    iiter_unique_ptr.reset(iiter);
  }

  // If needed the threshold could be more adaptive. For example, it can be
  // based on size, so that a larger will be sampled to more partitions than a
  // smaller file. The size might also need to be passed in by the caller based
  // on total compaction size.
  const uint64_t kMaxNumAnchors = uint64_t{128};
  uint64_t num_blocks = this->GetTableProperties()->num_data_blocks;
  uint64_t num_blocks_per_anchor = num_blocks / kMaxNumAnchors;
  if (num_blocks_per_anchor == 0) {
    num_blocks_per_anchor = 1;
  }

  uint64_t count = 0;
  std::string last_key;
  uint64_t range_size = 0;
  uint64_t prev_offset = 0;
  for (iiter->SeekToFirst(); iiter->Valid(); iiter->Next()) {
    const BlockHandle& bh = iiter->value().handle;
    range_size += bh.offset() + bh.size() - prev_offset;
    prev_offset = bh.offset() + bh.size();
    if (++count % num_blocks_per_anchor == 0) {
      count = 0;
      anchors.emplace_back(iiter->user_key(), range_size);
      range_size = 0;
    } else {
      last_key = iiter->user_key().ToString();
    }
  }
  if (count != 0) {
    anchors.emplace_back(last_key, range_size);
  }
  return Status::OK();
}

bool BlockBasedTable::TimestampMayMatch(const ReadOptions& read_options) const {
  if (read_options.timestamp != nullptr && !rep_->min_timestamp.empty()) {
    RecordTick(rep_->ioptions.stats, TIMESTAMP_FILTER_TABLE_CHECKED);
    auto read_ts = read_options.timestamp;
    auto comparator = rep_->internal_comparator.user_comparator();
    if (comparator->CompareTimestamp(*read_ts, rep_->min_timestamp) < 0) {
      RecordTick(rep_->ioptions.stats, TIMESTAMP_FILTER_TABLE_FILTERED);
      return false;
    }
  }
  return true;
}

Status BlockBasedTable::Get(const ReadOptions& read_options, const Slice& key,
                           GetContext* get_context,
                           const SliceTransform* prefix_extractor,
                           bool skip_filters) {
  // Similar to Bloom filter !may_match
  // If timestamp is beyond the range of the table, skip
  if (!TimestampMayMatch(read_options)) {
    return Status::OK();
  }
  assert(key.size() >= 8);  // key must be internal key
  assert(get_context != nullptr);
  Status s;

  FilterBlockReader* const filter =
      !skip_filters ? rep_->filter.get() : nullptr;

  // First check the full filter
  // If full filter not useful, Then go into each block
  uint64_t tracing_get_id = get_context->get_tracing_get_id();
  BlockCacheLookupContext lookup_context{
      TableReaderCaller::kUserGet, tracing_get_id,
      /*get_from_user_specified_snapshot=*/read_options.snapshot != nullptr};
  if (block_cache_tracer_ && block_cache_tracer_->is_tracing_enabled()) {
    // Trace the key since it contains both user key and sequence number.
    lookup_context.referenced_key = key.ToString();
    lookup_context.get_from_user_specified_snapshot =
        read_options.snapshot != nullptr;
  }
  TEST_SYNC_POINT("BlockBasedTable::Get:BeforeFilterMatch");
  const bool may_match =
      FullFilterKeyMayMatch(filter, key, prefix_extractor, get_context,
                            &lookup_context, read_options);
  TEST_SYNC_POINT("BlockBasedTable::Get:AfterFilterMatch");
  if (may_match) {
    IndexBlockIter iiter_on_stack;
    // if prefix_extractor found in block differs from options, disable
    // BlockPrefixIndex. Only do this check when index_type is kHashSearch.
    bool need_upper_bound_check = false;
    if (rep_->index_type == BlockBasedTableOptions::kHashSearch) {
      need_upper_bound_check = PrefixExtractorChanged(prefix_extractor);
    }
    auto iiter =
        NewIndexIterator(read_options, need_upper_bound_check, &iiter_on_stack,
                         get_context, &lookup_context);
    std::unique_ptr<InternalIteratorBase<IndexValue>> iiter_unique_ptr;
    if (iiter != &iiter_on_stack) {
      iiter_unique_ptr.reset(iiter);
    }

    size_t ts_sz =
        rep_->internal_comparator.user_comparator()->timestamp_size();
    bool matched = false;  // if such user key matched a key in SST
    bool done = false;
    std::unique_ptr<FilePrefetchBuffer> kvsep_prefetch_buffer;
    auto kvsep_fpb = [&]() -> FilePrefetchBuffer* {
      CreateExperimentalKVSepPrefetchBufferIfNeeded(
          rep_, read_options, &kvsep_prefetch_buffer,
          /*fallback_readahead_bytes=*/64 * 1024,
          FilePrefetchBufferUsage::kUnknown);
      return kvsep_prefetch_buffer.get();
    };
    for (iiter->Seek(key); iiter->Valid() && !done; iiter->Next()) {
      IndexValue v = iiter->value();

      if (!v.first_internal_key.empty() && !skip_filters &&
          UserComparatorWrapper(rep_->internal_comparator.user_comparator())
                  .CompareWithoutTimestamp(
                      ExtractUserKey(key),
                      ExtractUserKey(v.first_internal_key)) < 0) {
        // The requested key falls between highest key in previous block and
        // lowest key in current block.
        break;
      }

      BlockCacheLookupContext lookup_data_block_context{
          TableReaderCaller::kUserGet, tracing_get_id,
          /*get_from_user_specified_snapshot=*/read_options.snapshot !=
              nullptr};
      bool does_referenced_key_exist = false;
      DataBlockIter biter;
      uint64_t referenced_data_size = 0;
      Status tmp_status;
      const bool kvsep_enabled = rep_->experimental_kvsep_bptree_enabled;
      const bool kvsep_leaf_v2 =
          kvsep_enabled && rep_->kvsep_bptree_leaf_format_version == 2;
      const bool kvsep_pair_v3 =
          kvsep_enabled && rep_->kvsep_bptree_leaf_format_version == 3;

      if (kvsep_leaf_v2) {
        if (UNLIKELY(v.handle.IsNull())) {
          s = Status::Corruption("kvsep leaf v2: null leaf handle");
          break;
        }
        if (UNLIKELY(v.handle.offset() + v.handle.size() +
                         BlockBasedTable::kBlockTrailerSize >
                     rep_->file_size)) {
          s = Status::Corruption("kvsep leaf v2: leaf handle out of file range");
          break;
        }
        // Leaf V2: prefix-compressed leaf encoding (not a standard block KV
        // layout), so we cannot use DataBlockIter here.
        CachableEntry<Block_kKVSepLeaf> leaf_block;
        Status leaf_status = RetrieveBlock<Block_kKVSepLeaf>(
            /*prefetch_buffer=*/kvsep_fpb(), read_options, v.handle,
            rep_->decompressor.get(), &leaf_block, get_context,
            &lookup_data_block_context, /*for_compaction=*/false,
            /*use_cache=*/read_options.fill_cache, /*async_read=*/false,
            /*use_block_cache_for_lookup=*/true);
        if (read_options.read_tier == kBlockCacheTier &&
            leaf_status.IsIncomplete()) {
          get_context->MarkKeyMayExist();
          s = leaf_status;
          break;
        }
        if (!leaf_status.ok()) {
          s = leaf_status;
          break;
        }
        if (UNLIKELY(leaf_block.GetValue() == nullptr)) {
          s = Status::Corruption("kvsep leaf v2: missing leaf block");
          break;
        }
        if (UNLIKELY(leaf_block.GetValue()->ContentSlice().size() < 4)) {
          s = Status::Corruption(
              "kvsep leaf v2: leaf block too small: off=" +
              std::to_string(v.handle.offset()) +
              " size=" + std::to_string(v.handle.size()) +
              " block_bytes=" +
              std::to_string(leaf_block.GetValue()->ContentSlice().size()) +
              " key_hex=" + key.ToString(true));
          break;
        }
        KVSepBptreeLeafV2View leaf_view;
        Status parse_s =
            leaf_view.InitFromContents(leaf_block.GetValue()->ContentSlice());
        if (UNLIKELY(!parse_s.ok())) {
          s = parse_s;
          break;
        }

        // Lower bound in leaf.
        uint32_t left = 0;
        uint32_t right = leaf_view.num_entries();
        std::string mid_key_scratch;
        mid_key_scratch.reserve(64);
        while (left < right) {
          const uint32_t mid = left + (right - left) / 2;
          const Slice mid_key = leaf_view.FullKeyAt(mid, &mid_key_scratch);
          const int cmp = rep_->internal_comparator.Compare(mid_key, key);
          if (cmp < 0) {
            left = mid + 1;
          } else {
            right = mid;
          }
        }

        const Slice target_user_key = ExtractUserKey(key);
        CachableEntry<Block_kKVSepValue> kvsep_value_block;
        bool kvsep_value_block_loaded = false;
        BlockHandle kvsep_value_block_loaded_handle =
            BlockHandle::NullBlockHandle();
        std::string entry_key_scratch;
        entry_key_scratch.reserve(mid_key_scratch.capacity());
        for (uint32_t i = left; i < leaf_view.num_entries(); ++i) {
          const Slice entry_key = leaf_view.FullKeyAt(i, &entry_key_scratch);
          // If we've passed the user key, stop scanning this leaf.
          if (UserComparatorWrapper(rep_->internal_comparator.user_comparator())
                  .CompareWithoutTimestamp(ExtractUserKey(entry_key),
                                           target_user_key) > 0) {
            break;
          }

          ParsedInternalKey parsed_key;
          Status pik_status =
              ParseInternalKey(entry_key, &parsed_key, false /* log_err_key */);
          if (UNLIKELY(!pik_status.ok())) {
            s = pik_status;
            break;
          }

          Slice value_to_save;
          const uint32_t value_len = leaf_view.ValueLenAt(i);
          if (value_len == 0) {
            value_to_save = Slice();
          } else {
            const BlockHandle vb_handle = leaf_view.value_block_handle();
            if (UNLIKELY(vb_handle.IsNull())) {
              s = Status::Corruption("kvsep leaf v2: missing value block handle");
              break;
            }
            if (!kvsep_value_block_loaded ||
                kvsep_value_block_loaded_handle.offset() != vb_handle.offset() ||
                kvsep_value_block_loaded_handle.size() != vb_handle.size()) {
              Status vb_status = KVSepBptreeGetValueBlock(
                  read_options, vb_handle, &kvsep_value_block,
                  &lookup_data_block_context, kvsep_fpb());
              if (UNLIKELY(!vb_status.ok())) {
                s = vb_status;
                break;
              }
              kvsep_value_block_loaded = true;
              kvsep_value_block_loaded_handle = vb_handle;
            }
            if (UNLIKELY(kvsep_value_block.GetValue() == nullptr)) {
              s = Status::Corruption("kvsep missing value block");
              break;
            }
            const Slice value_block_contents =
                kvsep_value_block.GetValue()->ContentSlice();
            const uint32_t value_off = leaf_view.ValueOffAt(i);
            if (UNLIKELY(static_cast<size_t>(value_off) +
                             static_cast<size_t>(value_len) >
                         value_block_contents.size())) {
              s = Status::Corruption("kvsep leaf v2: value pointer out of range");
              break;
            }
            value_to_save =
                Slice(value_block_contents.data() + value_off, value_len);
          }

          Status read_status;
          bool ret =
              get_context->SaveValue(parsed_key, value_to_save, &matched,
                                     &read_status, /*value_pinner=*/nullptr);
          if (UNLIKELY(!read_status.ok())) {
            s = read_status;
            break;
          }
          if (!ret) {
            if (get_context->State() == GetContext::GetState::kFound) {
              does_referenced_key_exist = true;
              referenced_data_size = entry_key.size() + value_to_save.size();
            }
            done = true;
            break;
          }
        }
        if (!s.ok()) {
          break;
        }
      } else {
        if (kvsep_pair_v3) {
          if (UNLIKELY(v.handle.IsNull())) {
            s = Status::Corruption("kvsep pair v3: null pair handle");
            break;
          }
          if (UNLIKELY(v.handle.offset() + v.handle.size() +
                           BlockBasedTable::kBlockTrailerSize >
                       rep_->file_size)) {
            s = Status::Corruption(
                "kvsep pair v3: pair handle out of file range");
            break;
          }

          CachableEntry<Block_kKVSepPair> pair_block;
          Status pair_status = RetrieveBlock<Block_kKVSepPair>(
              /*prefetch_buffer=*/kvsep_fpb(), read_options, v.handle,
              rep_->decompressor.get(), &pair_block, get_context,
              &lookup_data_block_context, /*for_compaction=*/false,
              /*use_cache=*/read_options.fill_cache, /*async_read=*/false,
              /*use_block_cache_for_lookup=*/true);
          if (read_options.read_tier == kBlockCacheTier &&
              pair_status.IsIncomplete()) {
            get_context->MarkKeyMayExist();
            s = pair_status;
            break;
          }
          if (!pair_status.ok()) {
            s = pair_status;
            break;
          }
          if (UNLIKELY(pair_block.GetValue() == nullptr)) {
            s = Status::Corruption("kvsep pair v3: missing pair block");
            break;
          }

          KVSepBptreePairV3View pair_view;
          Status parse_pair =
              pair_view.InitFromContents(pair_block.GetValue()->ContentSlice());
          if (UNLIKELY(!parse_pair.ok())) {
            s = parse_pair;
            break;
          }
          KVSepBptreeLeafV3View leaf_view;
          Status parse_leaf = leaf_view.InitFromContents(pair_view.leaf_contents());
          if (UNLIKELY(!parse_leaf.ok())) {
            s = parse_leaf;
            break;
          }

          const size_t prefix_len = leaf_view.prefix().size();
          const Slice target_user_key = ExtractUserKey(key);
          const Slice value_block_contents = pair_view.value_contents();

          // Fast path: for bytewise comparator w/o timestamps, avoid materializing
          // full internal keys in the leaf binary search and version scan.
          const bool fast_bytewise =
              (ts_sz == 0) &&
              KVSepBptreeIsBytewiseComparator(
                  rep_->internal_comparator.user_comparator());

          uint32_t left = 0;
          uint32_t right = leaf_view.num_entries();
          if (fast_bytewise) {
            while (left < right) {
              const uint32_t mid = left + (right - left) / 2;
              const int cmp = KVSepBptreeCompareInternalKeyBytewise(
                  leaf_view.prefix(), leaf_view.SuffixAt(mid), key);
              if (cmp < 0) {
                left = mid + 1;
              } else {
                right = mid;
              }
            }
          } else {
            // Fallback: materialize full keys for comparator correctness.
            std::string mid_key_scratch;
            if (prefix_len > 0) {
              mid_key_scratch.assign(leaf_view.prefix().data(), prefix_len);
            }
            auto full_key_at = [&](uint32_t idx, std::string* scratch) -> Slice {
              if (prefix_len == 0) {
                return leaf_view.SuffixAt(idx);
              }
              const Slice suffix = leaf_view.SuffixAt(idx);
              scratch->resize(prefix_len + suffix.size());
              if (!suffix.empty()) {
                memcpy(&(*scratch)[prefix_len], suffix.data(), suffix.size());
              }
              return Slice(*scratch);
            };
            while (left < right) {
              const uint32_t mid = left + (right - left) / 2;
              const Slice mid_key = full_key_at(mid, &mid_key_scratch);
              const int cmp = rep_->internal_comparator.Compare(mid_key, key);
              if (cmp < 0) {
                left = mid + 1;
              } else {
                right = mid;
              }
            }
          }

          if (fast_bytewise) {
            for (uint32_t i = left; i < leaf_view.num_entries(); ++i) {
              const Slice suffix = leaf_view.SuffixAt(i);
              const int uk_cmp = KVSepBptreeCompareUserKeyBytewise(
                  leaf_view.prefix(), suffix, target_user_key);
              if (uk_cmp > 0) {
                break;
              }
              if (uk_cmp < 0) {
                continue;
              }

              uint64_t footer = 0;
              if (UNLIKELY(!KVSepBptreeDecodeInternalKeyFooter(
                      leaf_view.prefix(), suffix, &footer))) {
                s = Status::Corruption("kvsep pair v3: bad internal key footer");
                break;
              }

              ParsedInternalKey parsed_key;
              parsed_key.user_key = target_user_key;
              parsed_key.sequence = footer >> 8;
              parsed_key.type = static_cast<ValueType>(footer & 0xff);

              Slice value_to_save;
              const uint32_t value_len = leaf_view.ValueLenAt(i);
              if (value_len == 0) {
                value_to_save = Slice();
              } else {
                const uint32_t value_off = leaf_view.ValueOffAt(i);
                if (UNLIKELY(static_cast<size_t>(value_off) +
                                 static_cast<size_t>(value_len) >
                             value_block_contents.size())) {
                  s = Status::Corruption(
                      "kvsep pair v3: value pointer out of range");
                  break;
                }
                value_to_save =
                    Slice(value_block_contents.data() + value_off, value_len);
              }

              Status read_status;
              bool ret = get_context->SaveValue(parsed_key, value_to_save,
                                                &matched, &read_status,
                                                /*value_pinner=*/nullptr);
              if (UNLIKELY(!read_status.ok())) {
                s = read_status;
                break;
              }
              if (!ret) {
                if (get_context->State() == GetContext::GetState::kFound) {
                  does_referenced_key_exist = true;
                  referenced_data_size =
                      target_user_key.size() + value_to_save.size();
                }
                done = true;
                break;
              }
            }
          } else {
            // Fallback: materialize full keys and parse them.
            std::string entry_key_scratch;
            if (prefix_len > 0) {
              entry_key_scratch.assign(leaf_view.prefix().data(), prefix_len);
            }
            auto full_key_at = [&](uint32_t idx, std::string* scratch) -> Slice {
              if (prefix_len == 0) {
                return leaf_view.SuffixAt(idx);
              }
              const Slice suffix = leaf_view.SuffixAt(idx);
              scratch->resize(prefix_len + suffix.size());
              if (!suffix.empty()) {
                memcpy(&(*scratch)[prefix_len], suffix.data(), suffix.size());
              }
              return Slice(*scratch);
            };
            for (uint32_t i = left; i < leaf_view.num_entries(); ++i) {
              const Slice entry_key = full_key_at(i, &entry_key_scratch);
              // If we've passed the user key, stop scanning this leaf.
              if (UserComparatorWrapper(rep_->internal_comparator.user_comparator())
                      .CompareWithoutTimestamp(ExtractUserKey(entry_key),
                                               target_user_key) > 0) {
                break;
              }

              ParsedInternalKey parsed_key;
              Status pik_status = ParseInternalKey(
                  entry_key, &parsed_key, false /* log_err_key */);
              if (UNLIKELY(!pik_status.ok())) {
                s = pik_status;
                break;
              }

              Slice value_to_save;
              const uint32_t value_len = leaf_view.ValueLenAt(i);
              if (value_len == 0) {
                value_to_save = Slice();
              } else {
                const uint32_t value_off = leaf_view.ValueOffAt(i);
                if (UNLIKELY(static_cast<size_t>(value_off) +
                                 static_cast<size_t>(value_len) >
                             value_block_contents.size())) {
                  s = Status::Corruption(
                      "kvsep pair v3: value pointer out of range");
                  break;
                }
                value_to_save =
                    Slice(value_block_contents.data() + value_off, value_len);
              }

              Status read_status;
              bool ret = get_context->SaveValue(parsed_key, value_to_save,
                                                &matched, &read_status,
                                                /*value_pinner=*/nullptr);
              if (UNLIKELY(!read_status.ok())) {
                s = read_status;
                break;
              }
              if (!ret) {
                if (get_context->State() == GetContext::GetState::kFound) {
                  does_referenced_key_exist = true;
                  referenced_data_size =
                      entry_key.size() + value_to_save.size();
                }
                done = true;
                break;
              }
            }
          }
          if (!s.ok()) {
            break;
          }
        } else {
        if (kvsep_enabled) {
          KVSepBptreeNewLeafBlockIterator(
              read_options, v.handle, &biter, get_context,
              &lookup_data_block_context, /*prefetch_buffer=*/kvsep_fpb(),
              /*for_compaction=*/false, /*async_read=*/false, tmp_status,
              /*use_block_cache_for_lookup=*/true);
        } else {
          NewDataBlockIterator<DataBlockIter>(
              read_options, v.handle, &biter, BlockType::kData, get_context,
              &lookup_data_block_context, /*prefetch_buffer=*/nullptr,
              /*for_compaction=*/false, /*async_read=*/false, tmp_status,
              /*use_block_cache_for_lookup=*/true);
        }

        if (read_options.read_tier == kBlockCacheTier &&
            biter.status().IsIncomplete()) {
          // couldn't get block from block_cache
          // Update Saver.state to Found because we are only looking for
          // whether we can guarantee the key is not there when "no_io" is set
          get_context->MarkKeyMayExist();
          s = biter.status();
          break;
        }
        if (!biter.status().ok()) {
          s = biter.status();
          break;
        }

        bool may_exist = biter.SeekForGet(key);
        // If user-specified timestamp is supported, we cannot end the search
        // just because hash index lookup indicates the key+ts does not exist.
        if (!may_exist && ts_sz == 0) {
          // HashSeek cannot find the key this block and the the iter is not
          // the end of the block, i.e. cannot be in the following blocks
          // either. In this case, the seek_key cannot be found, so we break
          // from the top level for-loop.
          done = true;
        } else {
          // Call the *saver function on each entry/block until it returns false
          CachableEntry<Block_kKVSepValue> kvsep_value_block;
          bool kvsep_value_block_loaded = false;
          BlockHandle kvsep_value_block_loaded_handle =
              BlockHandle::NullBlockHandle();
          for (; biter.Valid(); biter.Next()) {
            ParsedInternalKey parsed_key;
            Status pik_status = ParseInternalKey(
                biter.key(), &parsed_key, false /* log_err_key */);  // TODO
            if (!pik_status.ok()) {
              s = pik_status;
              break;
            }

            Slice value_to_save = biter.value();
            if (kvsep_enabled) {
              BlockHandle kvsep_value_block_handle;
              uint32_t value_off = 0;
              uint32_t value_len = 0;
              Status decode_status = KVSepBptreeDecodePointer(
                  value_to_save, &kvsep_value_block_handle, &value_off,
                  &value_len);
              if (UNLIKELY(!decode_status.ok())) {
                s = decode_status;
                break;
              }
              if (value_len == 0) {
                value_to_save = Slice();
              } else {
	                if (!kvsep_value_block_loaded ||
	                    kvsep_value_block_loaded_handle.offset() !=
	                        kvsep_value_block_handle.offset() ||
	                    kvsep_value_block_loaded_handle.size() !=
	                        kvsep_value_block_handle.size()) {
	                  Status vb_status = KVSepBptreeGetValueBlock(
	                      read_options, kvsep_value_block_handle, &kvsep_value_block,
	                      &lookup_data_block_context, kvsep_fpb());
	                  if (UNLIKELY(!vb_status.ok())) {
	                    s = vb_status;
	                    break;
	                  }
                  kvsep_value_block_loaded = true;
                  kvsep_value_block_loaded_handle = kvsep_value_block_handle;
                }
                if (UNLIKELY(kvsep_value_block.GetValue() == nullptr)) {
                  s = Status::Corruption("kvsep missing value block");
                  break;
                }
                const Slice value_block_contents =
                    kvsep_value_block.GetValue()->ContentSlice();
                if (UNLIKELY(static_cast<size_t>(value_off) +
                                 static_cast<size_t>(value_len) >
                             value_block_contents.size())) {
                  s = Status::Corruption("kvsep pointer out of range");
                  break;
                }
                value_to_save = Slice(value_block_contents.data() + value_off,
                                      value_len);
              }
            }

            Status read_status;
            bool ret = get_context->SaveValue(
                parsed_key, value_to_save, &matched, &read_status,
                (kvsep_enabled || !biter.IsValuePinned()) ? nullptr : &biter);
            if (!read_status.ok()) {
              s = read_status;
              break;
            }
            if (!ret) {
              if (get_context->State() == GetContext::GetState::kFound) {
                does_referenced_key_exist = true;
                referenced_data_size = biter.key().size() + value_to_save.size();
              }
              done = true;
              break;
            }
          }
          if (s.ok()) {
            s = biter.status();
          }
          if (!s.ok()) {
            break;
          }
        }
      }
      }
      // Write the block cache access record.
      if (block_cache_tracer_ && block_cache_tracer_->is_tracing_enabled()) {
        // Avoid making copy of block_key, cf_name, and referenced_key when
        // constructing the access record.
        Slice referenced_key;
        if (does_referenced_key_exist) {
          referenced_key = (kvsep_leaf_v2 || kvsep_pair_v3) ? key : biter.key();
        } else {
          referenced_key = key;
        }
        FinishTraceRecord(lookup_data_block_context,
                          lookup_data_block_context.block_key, referenced_key,
                          does_referenced_key_exist, referenced_data_size);
      }

      if (done) {
        // Avoid the extra Next which is expensive in two-level indexes
        break;
      }
    }
    if (matched && filter != nullptr) {
      if (rep_->whole_key_filtering) {
        RecordTick(rep_->ioptions.stats, BLOOM_FILTER_FULL_TRUE_POSITIVE);
      } else {
        RecordTick(rep_->ioptions.stats, BLOOM_FILTER_PREFIX_TRUE_POSITIVE);
      }
      // Includes prefix stats
      PERF_COUNTER_BY_LEVEL_ADD(bloom_filter_full_true_positive, 1,
                                rep_->level);
    }

    if (s.ok() && !iiter->status().IsNotFound()) {
      s = iiter->status();
    }
  }

  return s;
}

Status BlockBasedTable::MultiGetFilter(const ReadOptions& read_options,
                                       const SliceTransform* prefix_extractor,
                                       MultiGetRange* mget_range) {
  if (mget_range->empty()) {
    // Caller should ensure non-empty (performance bug)
    assert(false);
    return Status::OK();  // Nothing to do
  }

  FilterBlockReader* const filter = rep_->filter.get();
  if (!filter) {
    return Status::OK();
  }

  // First check the full filter
  // If full filter not useful, Then go into each block
  uint64_t tracing_mget_id = BlockCacheTraceHelper::kReservedGetId;
  if (mget_range->begin()->get_context) {
    tracing_mget_id = mget_range->begin()->get_context->get_tracing_get_id();
  }
  BlockCacheLookupContext lookup_context{
      TableReaderCaller::kUserMultiGet, tracing_mget_id,
      /*_get_from_user_specified_snapshot=*/read_options.snapshot != nullptr};
  FullFilterKeysMayMatch(filter, mget_range, prefix_extractor, &lookup_context,
                         read_options);

  return Status::OK();
}

Status BlockBasedTable::Prefetch(const ReadOptions& read_options,
                                 const Slice* const begin,
                                 const Slice* const end) {
  auto& comparator = rep_->internal_comparator;
  UserComparatorWrapper user_comparator(comparator.user_comparator());
  // pre-condition
  if (begin && end && comparator.Compare(*begin, *end) > 0) {
    return Status::InvalidArgument(*begin, *end);
  }
  BlockCacheLookupContext lookup_context{TableReaderCaller::kPrefetch};
  IndexBlockIter iiter_on_stack;
  auto iiter = NewIndexIterator(read_options, /*disable_prefix_seek=*/false,
                                &iiter_on_stack, /*get_context=*/nullptr,
                                &lookup_context);
  std::unique_ptr<InternalIteratorBase<IndexValue>> iiter_unique_ptr;
  if (iiter != &iiter_on_stack) {
    iiter_unique_ptr = std::unique_ptr<InternalIteratorBase<IndexValue>>(iiter);
  }

  if (!iiter->status().ok()) {
    // error opening index iterator
    return iiter->status();
  }

  // indicates if we are on the last page that need to be pre-fetched
  bool prefetching_boundary_page = false;

  for (begin ? iiter->Seek(*begin) : iiter->SeekToFirst(); iiter->Valid();
       iiter->Next()) {
    BlockHandle block_handle = iiter->value().handle;
    const bool is_user_key = !rep_->index_key_includes_seq;
    if (end &&
        ((!is_user_key && comparator.Compare(iiter->key(), *end) >= 0) ||
         (is_user_key &&
          user_comparator.Compare(iiter->key(), ExtractUserKey(*end)) >= 0))) {
      if (prefetching_boundary_page) {
        break;
      }

      // The index entry represents the last key in the data block.
      // We should load this page into memory as well, but no more
      prefetching_boundary_page = true;
    }

    // Load the block specified by the block_handle into the block cache
    DataBlockIter biter;
    Status tmp_status;
    NewDataBlockIterator<DataBlockIter>(
        read_options, block_handle, &biter, /*block_type=*/BlockType::kData,
        /*get_context=*/nullptr, &lookup_context,
        /*prefetch_buffer=*/nullptr, /*for_compaction=*/false,
        /*async_read=*/false, tmp_status, /*use_block_cache_for_lookup=*/true);

    if (!biter.status().ok()) {
      // there was an unexpected error while pre-fetching
      return biter.status();
    }
  }

  return Status::OK();
}

Status BlockBasedTable::VerifyChecksum(const ReadOptions& read_options,
                                       TableReaderCaller caller,
                                       bool meta_blocks_only) {
  Status s;
  // Check Meta blocks
  std::unique_ptr<Block> metaindex;
  std::unique_ptr<InternalIterator> metaindex_iter;
  s = ReadMetaIndexBlock(read_options, nullptr /* prefetch buffer */,
                         &metaindex, &metaindex_iter);
  if (s.ok()) {
    s = VerifyChecksumInMetaBlocks(read_options, metaindex_iter.get());
    if (!s.ok()) {
      return s;
    }
  } else {
    return s;
  }
  if (meta_blocks_only) {
    return s;
  }
  // Check Data blocks
  IndexBlockIter iiter_on_stack;
  BlockCacheLookupContext context{caller};
  InternalIteratorBase<IndexValue>* iiter = NewIndexIterator(
      read_options, /*disable_prefix_seek=*/false, &iiter_on_stack,
      /*get_context=*/nullptr, &context);
  std::unique_ptr<InternalIteratorBase<IndexValue>> iiter_unique_ptr;
  if (iiter != &iiter_on_stack) {
    iiter_unique_ptr = std::unique_ptr<InternalIteratorBase<IndexValue>>(iiter);
  }
  if (!iiter->status().ok()) {
    // error opening index iterator
    return iiter->status();
  }
  s = VerifyChecksumInBlocks(read_options, iiter);
  return s;
}

Status BlockBasedTable::VerifyChecksumInBlocks(
    const ReadOptions& read_options,
    InternalIteratorBase<IndexValue>* index_iter) {
  Status s;
  // We are scanning the whole file, so no need to do exponential
  // increasing of the buffer size.
  size_t readahead_size = (read_options.readahead_size != 0)
                              ? read_options.readahead_size
                              : rep_->table_options.max_auto_readahead_size;
  // FilePrefetchBuffer doesn't work in mmap mode and readahead is not
  // needed there.
  ReadaheadParams readahead_params;
  readahead_params.initial_readahead_size = readahead_size;
  readahead_params.max_readahead_size = readahead_size;
  FilePrefetchBuffer prefetch_buffer(
      readahead_params, !rep_->ioptions.allow_mmap_reads /* enable */);

  for (index_iter->SeekToFirst(); index_iter->Valid(); index_iter->Next()) {
    s = index_iter->status();
    if (!s.ok()) {
      break;
    }
    BlockHandle handle = index_iter->value().handle;
    BlockContents contents;
    BlockFetcher block_fetcher(
        rep_->file.get(), &prefetch_buffer, rep_->footer, read_options, handle,
        &contents, rep_->table_options.super_block_alignment_size,
        rep_->table_options.enable_super_block_read_coalescing,
        rep_->ioptions, false /* decompress */,
        false /*maybe_compressed*/, BlockType::kData, nullptr /*decompressor*/,
        rep_->persistent_cache_options);
    s = block_fetcher.ReadBlockContents();
    if (!s.ok()) {
      break;
    }
  }
  if (s.ok()) {
    // In the case of two level indexes, we would have exited the above loop
    // by checking index_iter->Valid(), but Valid() might have returned false
    // due to an IO error. So check the index_iter status
    s = index_iter->status();
  }
  return s;
}

BlockType BlockBasedTable::GetBlockTypeForMetaBlockByName(
    const Slice& meta_block_name) {
  if (meta_block_name.starts_with(kFullFilterBlockPrefix)) {
    return BlockType::kFilter;
  }

  if (meta_block_name.starts_with(kPartitionedFilterBlockPrefix)) {
    return BlockType::kFilterPartitionIndex;
  }

  if (meta_block_name == kPropertiesBlockName) {
    return BlockType::kProperties;
  }

  if (meta_block_name == kCompressionDictBlockName) {
    return BlockType::kCompressionDictionary;
  }

  if (meta_block_name == kRangeDelBlockName) {
    return BlockType::kRangeDeletion;
  }

  if (meta_block_name == kHashIndexPrefixesBlock) {
    return BlockType::kHashIndexPrefixes;
  }

  if (meta_block_name == kHashIndexPrefixesMetadataBlock) {
    return BlockType::kHashIndexMetadata;
  }

  if (meta_block_name == kIndexBlockName) {
    return BlockType::kIndex;
  }

  if (meta_block_name.starts_with(kUserDefinedIndexPrefix)) {
    return BlockType::kUserDefinedIndex;
  }

  if (meta_block_name.starts_with(kObsoleteFilterBlockPrefix)) {
    // Obsolete but possible in old files
    return BlockType::kInvalid;
  }

  assert(false);
  return BlockType::kInvalid;
}

Status BlockBasedTable::VerifyChecksumInMetaBlocks(
    const ReadOptions& read_options, InternalIteratorBase<Slice>* index_iter) {
  Status s;
  for (index_iter->SeekToFirst(); index_iter->Valid(); index_iter->Next()) {
    s = index_iter->status();
    if (!s.ok()) {
      break;
    }
    BlockHandle handle;
    Slice input = index_iter->value();
    s = handle.DecodeFrom(&input);
    if (!s.ok()) {
      break;
    }
    BlockContents contents;
    const Slice meta_block_name = index_iter->key();
    if (meta_block_name == kPropertiesBlockName) {
      // Unfortunate special handling for properties block checksum w/
      // global seqno
      std::unique_ptr<TableProperties> table_properties;
      s = ReadTablePropertiesHelper(read_options, handle, rep_->file.get(),
                                    nullptr /* prefetch_buffer */, rep_->footer,
                                    rep_->ioptions, &table_properties,
                                    nullptr /* memory_allocator */);
    } else if (rep_->verify_checksum_set_on_open &&
               meta_block_name == kIndexBlockName) {
      // WART: For now, to maintain similar I/O behavior as before
      // format_version=6, we skip verifying index block checksum--but only
      // if it was checked on open.
    } else {
      // FIXME? Need to verify checksums of index and filter partitions?
      s = BlockFetcher(rep_->file.get(), nullptr /* prefetch buffer */,
                       rep_->footer, read_options, handle, &contents,
                       rep_->table_options.super_block_alignment_size,
                       rep_->table_options.enable_super_block_read_coalescing,
                       rep_->ioptions, false /* decompress */,
                       false /*maybe_compressed*/,
                       GetBlockTypeForMetaBlockByName(meta_block_name),
                       nullptr /*decompressor*/, rep_->persistent_cache_options)
              .ReadBlockContents();
    }
    if (!s.ok()) {
      break;
    }
  }
  return s;
}

bool BlockBasedTable::EraseFromCache(const BlockHandle& handle) const {
  assert(rep_ != nullptr);

  Cache* const cache = rep_->table_options.block_cache.get();
  if (cache == nullptr) {
    return false;
  }

  CacheKey key = GetCacheKey(rep_->base_cache_key, handle);

  Cache::Handle* const cache_handle = cache->Lookup(key.AsSlice());
  if (cache_handle == nullptr) {
    return false;
  }

  return cache->Release(cache_handle, /*erase_if_last_ref=*/true);
}

bool BlockBasedTable::TEST_BlockInCache(const BlockHandle& handle) const {
  assert(rep_ != nullptr);

  Cache* const cache = rep_->table_options.block_cache.get();
  if (cache == nullptr) {
    return false;
  }

  CacheKey key = GetCacheKey(rep_->base_cache_key, handle);

  Cache::Handle* const cache_handle = cache->Lookup(key.AsSlice());
  if (cache_handle == nullptr) {
    return false;
  }

  cache->Release(cache_handle);

  return true;
}

bool BlockBasedTable::TEST_KeyInCache(const ReadOptions& options,
                                      const Slice& key) {
  std::unique_ptr<InternalIteratorBase<IndexValue>> iiter(NewIndexIterator(
      options, /*disable_prefix_seek=*/false, /*input_iter=*/nullptr,
      /*get_context=*/nullptr, /*lookup_context=*/nullptr));
  iiter->Seek(key);
  assert(iiter->status().ok());
  assert(iiter->Valid());

  return TEST_BlockInCache(iiter->value().handle);
}

void BlockBasedTable::TEST_GetDataBlockHandle(const ReadOptions& options,
                                              const Slice& key,
                                              BlockHandle& handle) {
  std::unique_ptr<InternalIteratorBase<IndexValue>> iiter(NewIndexIterator(
      options, /*disable_prefix_seek=*/false, /*input_iter=*/nullptr,
      /*get_context=*/nullptr, /*lookup_context=*/nullptr));
  iiter->Seek(key);
  assert(iiter->Valid());
  handle = iiter->value().handle;
}

// REQUIRES: The following fields of rep_ should have already been populated:
//  1. file
//  2. index_handle,
//  3. options
//  4. internal_comparator
//  5. index_type
Status BlockBasedTable::CreateIndexReader(
    const ReadOptions& ro, FilePrefetchBuffer* prefetch_buffer,
    InternalIterator* meta_iter, bool use_cache, bool prefetch, bool pin,
    BlockCacheLookupContext* lookup_context,
    std::unique_ptr<IndexReader>* index_reader) {
  if (FormatVersionUsesIndexHandleInFooter(rep_->footer.format_version())) {
    rep_->index_handle = rep_->footer.index_handle();
  } else {
    Status s = FindMetaBlock(meta_iter, kIndexBlockName, &rep_->index_handle);
    if (!s.ok()) {
      return s;
    }
  }

  if (rep_->experimental_kvsep_bptree_enabled) {
    return KVSepBptreeIndexReader::Create(this, ro, prefetch_buffer, use_cache,
                                          prefetch, pin, lookup_context,
                                          index_reader);
  }

  switch (rep_->index_type) {
    case BlockBasedTableOptions::kTwoLevelIndexSearch: {
      return PartitionIndexReader::Create(this, ro, prefetch_buffer, use_cache,
                                          prefetch, pin, lookup_context,
                                          index_reader);
    }
    case BlockBasedTableOptions::kBinarySearch:
      FALLTHROUGH_INTENDED;
    case BlockBasedTableOptions::kBinarySearchWithFirstKey: {
      return BinarySearchIndexReader::Create(this, ro, prefetch_buffer,
                                             use_cache, prefetch, pin,
                                             lookup_context, index_reader);
    }
    case BlockBasedTableOptions::kHashSearch: {
      if (!rep_->table_prefix_extractor) {
        ROCKS_LOG_WARN(rep_->ioptions.logger,
                       "Missing prefix extractor for hash index. Fall back to"
                       " binary search index.");
        return BinarySearchIndexReader::Create(this, ro, prefetch_buffer,
                                               use_cache, prefetch, pin,
                                               lookup_context, index_reader);
      } else {
        return HashIndexReader::Create(this, ro, prefetch_buffer, meta_iter,
                                       use_cache, prefetch, pin, lookup_context,
                                       index_reader);
      }
    }
    default: {
      std::string error_message =
          "Unrecognized index type: " + std::to_string(rep_->index_type);
      return Status::InvalidArgument(error_message.c_str());
    }
  }
}

uint64_t BlockBasedTable::ApproximateDataOffsetOf(
    const InternalIteratorBase<IndexValue>& index_iter,
    uint64_t data_size) const {
  assert(index_iter.status().ok());
  if (index_iter.Valid()) {
    BlockHandle handle = index_iter.value().handle;
    return handle.offset();
  } else {
    // The iterator is past the last key in the file.
    return data_size;
  }
}

uint64_t BlockBasedTable::GetApproximateDataSize() {
  // Should be in table properties unless super old version
  if (rep_->table_properties) {
    return rep_->table_properties->data_size;
  }
  // Fall back to rough estimate from footer
  return rep_->footer.metaindex_handle().offset();
}

uint64_t BlockBasedTable::ApproximateOffsetOf(const ReadOptions& read_options,
                                              const Slice& key,
                                              TableReaderCaller caller) {
  uint64_t data_size = GetApproximateDataSize();
  if (UNLIKELY(data_size == 0)) {
    // Hmm. Let's just split in half to avoid skewing one way or another,
    // since we don't know whether we're operating on lower bound or
    // upper bound.
    return rep_->file_size / 2;
  }

  BlockCacheLookupContext context(caller);
  IndexBlockIter iiter_on_stack;
  auto index_iter =
      NewIndexIterator(read_options, /*disable_prefix_seek=*/true,
                       /*input_iter=*/&iiter_on_stack, /*get_context=*/nullptr,
                       /*lookup_context=*/&context);
  std::unique_ptr<InternalIteratorBase<IndexValue>> iiter_unique_ptr;
  if (index_iter != &iiter_on_stack) {
    iiter_unique_ptr.reset(index_iter);
  }

  index_iter->Seek(key);
  uint64_t offset;
  if (index_iter->status().ok()) {
    offset = ApproximateDataOffsetOf(*index_iter, data_size);
  } else {
    // Split in half to avoid skewing one way or another,
    // since we don't know whether we're operating on lower bound or
    // upper bound.
    return rep_->file_size / 2;
  }

  // Pro-rate file metadata (incl filters) size-proportionally across data
  // blocks.
  double size_ratio =
      static_cast<double>(offset) / static_cast<double>(data_size);
  return static_cast<uint64_t>(size_ratio *
                               static_cast<double>(rep_->file_size));
}

uint64_t BlockBasedTable::ApproximateSize(const ReadOptions& read_options,
                                          const Slice& start, const Slice& end,
                                          TableReaderCaller caller) {
  assert(rep_->internal_comparator.Compare(start, end) <= 0);

  uint64_t data_size = GetApproximateDataSize();
  if (UNLIKELY(data_size == 0)) {
    // Hmm. Assume whole file is involved, since we have lower and upper
    // bound. This likely skews the estimate if we consider that this function
    // is typically called with `[start, end]` fully contained in the file's
    // key-range.
    return rep_->file_size;
  }

  BlockCacheLookupContext context(caller);
  IndexBlockIter iiter_on_stack;
  auto index_iter =
      NewIndexIterator(read_options, /*disable_prefix_seek=*/true,
                       /*input_iter=*/&iiter_on_stack, /*get_context=*/nullptr,
                       /*lookup_context=*/&context);
  std::unique_ptr<InternalIteratorBase<IndexValue>> iiter_unique_ptr;
  if (index_iter != &iiter_on_stack) {
    iiter_unique_ptr.reset(index_iter);
  }

  index_iter->Seek(start);
  uint64_t start_offset;
  if (index_iter->status().ok()) {
    start_offset = ApproximateDataOffsetOf(*index_iter, data_size);
  } else {
    // Assume file is involved from the start. This likely skews the estimate
    // but is consistent with the above error handling.
    start_offset = 0;
  }

  index_iter->Seek(end);
  uint64_t end_offset;
  if (index_iter->status().ok()) {
    end_offset = ApproximateDataOffsetOf(*index_iter, data_size);
  } else {
    // Assume file is involved until the end. This likely skews the estimate
    // but is consistent with the above error handling.
    end_offset = data_size;
  }

  assert(end_offset >= start_offset);
  // Pro-rate file metadata (incl filters) size-proportionally across data
  // blocks.
  double size_ratio = static_cast<double>(end_offset - start_offset) /
                      static_cast<double>(data_size);
  return static_cast<uint64_t>(size_ratio *
                               static_cast<double>(rep_->file_size));
}

bool BlockBasedTable::TEST_FilterBlockInCache() const {
  assert(rep_ != nullptr);
  return rep_->filter_type != Rep::FilterType::kNoFilter &&
         TEST_BlockInCache(rep_->filter_handle);
}

bool BlockBasedTable::TEST_IndexBlockInCache() const {
  assert(rep_ != nullptr);

  return TEST_BlockInCache(rep_->index_handle);
}

Status BlockBasedTable::GetKVPairsFromDataBlocks(
    const ReadOptions& read_options, std::vector<KVPairBlock>* kv_pair_blocks) {
  std::unique_ptr<InternalIteratorBase<IndexValue>> blockhandles_iter(
      NewIndexIterator(read_options, /*disable_prefix_seek=*/false,
                       /*input_iter=*/nullptr, /*get_context=*/nullptr,
                       /*lookup_context=*/nullptr));

  Status s = blockhandles_iter->status();
  if (!s.ok()) {
    // Cannot read Index Block
    return s;
  }

  for (blockhandles_iter->SeekToFirst(); blockhandles_iter->Valid();
       blockhandles_iter->Next()) {
    s = blockhandles_iter->status();

    if (!s.ok()) {
      break;
    }

    std::unique_ptr<InternalIterator> datablock_iter;
    Status tmp_status;
    datablock_iter.reset(NewDataBlockIterator<DataBlockIter>(
        read_options, blockhandles_iter->value().handle,
        /*input_iter=*/nullptr, /*block_type=*/BlockType::kData,
        /*get_context=*/nullptr, /*lookup_context=*/nullptr,
        /*prefetch_buffer=*/nullptr, /*for_compaction=*/false,
        /*async_read=*/false, tmp_status, /*use_block_cache_for_lookup=*/true));
    s = datablock_iter->status();

    if (!s.ok()) {
      // Error reading the block - Skipped
      continue;
    }

    KVPairBlock kv_pair_block;
    for (datablock_iter->SeekToFirst(); datablock_iter->Valid();
         datablock_iter->Next()) {
      s = datablock_iter->status();
      if (!s.ok()) {
        // Error reading the block - Skipped
        break;
      }
      const Slice& key = datablock_iter->key();
      const Slice& value = datablock_iter->value();
      std::string key_copy = std::string(key.data(), key.size());
      std::string value_copy = std::string(value.data(), value.size());

      kv_pair_block.push_back(
          std::make_pair(std::move(key_copy), std::move(value_copy)));
    }
    kv_pair_blocks->push_back(std::move(kv_pair_block));
  }
  return Status::OK();
}

Status BlockBasedTable::DumpTable(WritableFile* out_file,
                                  bool show_sequence_number_type) {
  WritableFileStringStreamAdapter out_file_wrapper(out_file);
  std::ostream out_stream(&out_file_wrapper);
  // Output Footer
  out_stream << "Footer Details:\n"
                "--------------------------------------\n";
  out_stream << "  " << rep_->footer.ToString() << "\n";

  // Output MetaIndex
  out_stream << "Metaindex Details:\n"
                "--------------------------------------\n";
  std::unique_ptr<Block> metaindex;
  std::unique_ptr<InternalIterator> metaindex_iter;
  // TODO: plumb Env::IOActivity, Env::IOPriority
  const ReadOptions ro;
  Status s = ReadMetaIndexBlock(ro, nullptr /* prefetch_buffer */, &metaindex,
                                &metaindex_iter);
  if (s.ok()) {
    for (metaindex_iter->SeekToFirst(); metaindex_iter->Valid();
         metaindex_iter->Next()) {
      s = metaindex_iter->status();
      if (!s.ok()) {
        return s;
      }
      if (metaindex_iter->key() == kPropertiesBlockName) {
        out_stream << "  Properties block handle: "
                   << metaindex_iter->value().ToString(true) << "\n";
      } else if (metaindex_iter->key() == kCompressionDictBlockName) {
        out_stream << "  Compression dictionary block handle: "
                   << metaindex_iter->value().ToString(true) << "\n";
      } else if (strstr(metaindex_iter->key().ToString().c_str(),
                        "filter.rocksdb.") != nullptr) {
        out_stream << "  Filter block handle: "
                   << metaindex_iter->value().ToString(true) << "\n";
      } else if (metaindex_iter->key() == kRangeDelBlockName) {
        out_stream << "  Range deletion block handle: "
                   << metaindex_iter->value().ToString(true) << "\n";
      }
    }
    out_stream << "\n";
  } else {
    return s;
  }

  // Output TableProperties
  const ROCKSDB_NAMESPACE::TableProperties* table_properties;
  table_properties = rep_->table_properties.get();

  if (table_properties != nullptr) {
    out_stream << "Table Properties:\n"
                  "--------------------------------------\n";
    out_stream << "  " << table_properties->ToString("\n  ", ": ") << "\n";
  }

  if (rep_->filter) {
    out_stream << "Filter Details:\n"
                  "--------------------------------------\n";
    out_stream << "  " << rep_->filter->ToString() << "\n";
  }

  // Output Index block
  s = DumpIndexBlock(out_stream);
  if (!s.ok()) {
    return s;
  }

  // Output compression dictionary
  if (rep_->uncompression_dict_reader) {
    CachableEntry<DecompressorDict> uncompression_dict;
    s = rep_->uncompression_dict_reader->GetOrReadUncompressionDictionary(
        nullptr /* prefetch_buffer */, ro, nullptr /* get_context */,
        nullptr /* lookup_context */, &uncompression_dict);
    if (!s.ok()) {
      return s;
    }

    assert(uncompression_dict.GetValue());

    const Slice& raw_dict = uncompression_dict.GetValue()->GetRawDict();
    out_stream << "Compression Dictionary:\n"
                  "--------------------------------------\n";
    out_stream << "  size (bytes): " << raw_dict.size() << "\n\n";
    out_stream << "  HEX    " << raw_dict.ToString(true) << "\n\n";
  }

  // Output range deletions block
  auto* range_del_iter = NewRangeTombstoneIterator(ro);
  if (range_del_iter != nullptr) {
    range_del_iter->SeekToFirst();
    if (range_del_iter->Valid()) {
      out_stream << "Range deletions:\n"
                    "--------------------------------------\n";
      for (; range_del_iter->Valid(); range_del_iter->Next()) {
        DumpKeyValue(range_del_iter->key(), range_del_iter->value(), out_stream,
                     show_sequence_number_type);
      }
      out_stream << "\n";
    }
    delete range_del_iter;
  }
  // Output Data blocks
  s = DumpDataBlocks(out_stream, show_sequence_number_type);

  if (!s.ok()) {
    return s;
  }

  if (!out_stream.good()) {
    return Status::IOError("Failed to write to output file");
  }
  return Status::OK();
}

Status BlockBasedTable::DumpIndexBlock(std::ostream& out_stream) {
  out_stream << "Index Details:\n"
                "--------------------------------------\n";
  // TODO: plumb Env::IOActivity, Env::IOPriority
  const ReadOptions read_options;
  std::unique_ptr<InternalIteratorBase<IndexValue>> blockhandles_iter(
      NewIndexIterator(read_options, /*disable_prefix_seek=*/false,
                       /*input_iter=*/nullptr, /*get_context=*/nullptr,
                       /*lookup_context=*/nullptr));
  Status s = blockhandles_iter->status();
  if (!s.ok()) {
    out_stream << "Can not read Index Block \n\n";
    return s;
  }

  out_stream << "  Block key hex dump: Data block handle\n";
  out_stream << "  Block key ascii\n\n";
  for (blockhandles_iter->SeekToFirst(); blockhandles_iter->Valid();
       blockhandles_iter->Next()) {
    s = blockhandles_iter->status();
    if (!s.ok()) {
      break;
    }
    Slice key = blockhandles_iter->key();
    Slice user_key;
    InternalKey ikey;
    if (!rep_->index_key_includes_seq) {
      user_key = key;
    } else {
      ikey.DecodeFrom(key);
      user_key = ikey.user_key();
    }

    out_stream << "  HEX    " << user_key.ToString(true) << ": "
               << blockhandles_iter->value().ToString(true,
                                                      rep_->index_has_first_key)
               << " offset " << blockhandles_iter->value().handle.offset()
               << " size " << blockhandles_iter->value().handle.size() << "\n";

    std::string str_key = user_key.ToString();
    std::string res_key;
    char cspace = ' ';
    for (size_t i = 0; i < str_key.size(); i++) {
      res_key.append(&str_key[i], 1);
      res_key.append(1, cspace);
    }
    out_stream << "  ASCII  " << res_key << "\n";
    out_stream << "  ------\n";
  }
  out_stream << "\n";
  return Status::OK();
}

Status BlockBasedTable::DumpDataBlocks(std::ostream& out_stream,
                                       bool show_sequence_number_type) {
  // TODO: plumb Env::IOActivity, Env::IOPriority
  const ReadOptions read_options;
  std::unique_ptr<InternalIteratorBase<IndexValue>> blockhandles_iter(
      NewIndexIterator(read_options, /*disable_prefix_seek=*/false,
                       /*input_iter=*/nullptr, /*get_context=*/nullptr,
                       /*lookup_context=*/nullptr));
  Status s = blockhandles_iter->status();
  if (!s.ok()) {
    out_stream << "Can not read Index Block \n\n";
    return s;
  }

  uint64_t datablock_size_min = std::numeric_limits<uint64_t>::max();
  uint64_t datablock_size_max = 0;
  uint64_t datablock_size_sum = 0;

  size_t block_id = 1;
  for (blockhandles_iter->SeekToFirst(); blockhandles_iter->Valid();
       block_id++, blockhandles_iter->Next()) {
    s = blockhandles_iter->status();
    if (!s.ok()) {
      break;
    }

    BlockHandle bh = blockhandles_iter->value().handle;
    uint64_t datablock_size = bh.size();
    datablock_size_min = std::min(datablock_size_min, datablock_size);
    datablock_size_max = std::max(datablock_size_max, datablock_size);
    datablock_size_sum += datablock_size;

    out_stream << "Data Block # " << block_id << " @ "
               << blockhandles_iter->value().handle.ToString(true) << "\n";
    out_stream << "--------------------------------------\n";

    std::unique_ptr<InternalIterator> datablock_iter;
    Status tmp_status;
    datablock_iter.reset(NewDataBlockIterator<DataBlockIter>(
        read_options, blockhandles_iter->value().handle,
        /*input_iter=*/nullptr, /*block_type=*/BlockType::kData,
        /*get_context=*/nullptr, /*lookup_context=*/nullptr,
        /*prefetch_buffer=*/nullptr, /*for_compaction=*/false,
        /*async_read=*/false, tmp_status, /*use_block_cache_for_lookup=*/true));
    s = datablock_iter->status();

    if (!s.ok()) {
      out_stream << "Error reading the block - Skipped \n\n";
      continue;
    }

    for (datablock_iter->SeekToFirst(); datablock_iter->Valid();
         datablock_iter->Next()) {
      s = datablock_iter->status();
      if (!s.ok()) {
        out_stream << "Error reading the block - Skipped \n";
        break;
      }
      DumpKeyValue(datablock_iter->key(), datablock_iter->value(), out_stream,
                   show_sequence_number_type);
    }
    out_stream << "\n";
  }

  uint64_t num_datablocks = block_id - 1;
  if (num_datablocks) {
    double datablock_size_avg =
        static_cast<double>(datablock_size_sum) / num_datablocks;
    out_stream << "Data Block Summary:\n";
    out_stream << "--------------------------------------\n";
    out_stream << "  # data blocks: " << num_datablocks << "\n";
    out_stream << "  min data block size: " << datablock_size_min << "\n";
    out_stream << "  max data block size: " << datablock_size_max << "\n";
    out_stream << "  avg data block size: "
               << std::to_string(datablock_size_avg) << "\n";
  }

  return Status::OK();
}

void BlockBasedTable::DumpKeyValue(const Slice& key, const Slice& value,
                                   std::ostream& out_stream,
                                   bool show_sequence_number_type) {
  ParsedInternalKey result;
  auto s = ParseInternalKey(key, &result, true);
  if (!s.ok()) {
    out_stream << "Error parsing internal key - Skipped \n";
    return;
  }

  if (show_sequence_number_type) {
    out_stream << "  HEX    " << result.user_key.ToString(true)
               << "  seq: " << result.sequence
               << "  type: " << std::to_string(result.type) << " : "
               << value.ToString(true) << "\n";
  } else {
    out_stream << "  HEX    " << result.user_key.ToString(true) << ": "
               << value.ToString(true) << "\n";
  }

  std::string str_key = result.user_key.ToString();
  std::string str_value = value.ToString();
  std::string res_key, res_value;
  char cspace = ' ';
  for (size_t i = 0; i < str_key.size(); i++) {
    if (str_key[i] == '\0') {
      res_key.append("\\0", 2);
    } else {
      res_key.append(&str_key[i], 1);
    }
    res_key.append(1, cspace);
  }
  for (size_t i = 0; i < str_value.size(); i++) {
    if (str_value[i] == '\0') {
      res_value.append("\\0", 2);
    } else {
      res_value.append(&str_value[i], 1);
    }
    res_value.append(1, cspace);
  }

  out_stream << "  ASCII  " << res_key << ": " << res_value << "\n";
  out_stream << "  ------\n";
}

void BlockBasedTable::MarkObsolete(uint32_t uncache_aggressiveness) {
  rep_->uncache_aggressiveness.StoreRelaxed(uncache_aggressiveness);
}

}  // namespace ROCKSDB_NAMESPACE
