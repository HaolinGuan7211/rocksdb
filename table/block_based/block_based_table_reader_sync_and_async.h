//  Copyright (c) Meta Platforms, Inc. and affiliates.
//
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include <algorithm>
#include <memory>

#include "util/async_file_reader.h"
#include "util/coro_utils.h"

#if defined(WITHOUT_COROUTINES) || \
    (defined(USE_COROUTINES) && defined(WITH_COROUTINES))

namespace ROCKSDB_NAMESPACE {

// This function reads multiple data blocks from disk using Env::MultiRead()
// and optionally inserts them into the block cache. It uses the scratch
// buffer provided by the caller, which is contiguous. If scratch is a nullptr
// it allocates a separate buffer for each block. Typically, if the blocks
// need to be uncompressed and there is no compressed block cache, callers
// can allocate a temporary scratch buffer in order to minimize memory
// allocations.
// If options.fill_cache is true, it inserts the blocks into cache. If its
// false and scratch is non-null and the blocks are uncompressed, it copies
// the buffers to heap. In any case, the CachableEntry<Block> returned will
// own the data bytes.
// If compression is enabled and also there is no compressed block cache,
// the adjacent blocks are read out in one IO (combined read)
// batch - A MultiGetRange with only those keys with unique data blocks not
//         found in cache
// handles - A vector of block handles. Some of them me be NULL handles
// scratch - An optional contiguous buffer to read compressed blocks into
DEFINE_SYNC_AND_ASYNC(void, BlockBasedTable::RetrieveMultipleBlocks)
(const ReadOptions& options, const MultiGetRange* batch,
 const autovector<BlockHandle, MultiGetContext::MAX_BATCH_SIZE>* handles,
 Status* statuses, CachableEntry<Block_kData>* results, char* scratch,
 UnownedPtr<Decompressor> decomp, bool use_fs_scratch) const {
  RandomAccessFileReader* file = rep_->file.get();
  const Footer& footer = rep_->footer;
  const ImmutableOptions& ioptions = rep_->ioptions;

  if (ioptions.allow_mmap_reads) {
    size_t idx_in_batch = 0;
    for (auto mget_iter = batch->begin(); mget_iter != batch->end();
         ++mget_iter, ++idx_in_batch) {
      const BlockHandle& handle = (*handles)[idx_in_batch];
      if (handle.IsNull()) {
        continue;
      }

      // XXX: use_cache=true means double cache query?
      statuses[idx_in_batch] = RetrieveBlock(
          nullptr, options, handle, decomp,
          &results[idx_in_batch].As<Block_kData>(), mget_iter->get_context,
          /* lookup_context */ nullptr,
          /* for_compaction */ false, /* use_cache */ true,
          /* async_read */ false, /* use_block_cache_for_lookup */ true);
    }
    assert(idx_in_batch == handles->size());
    CO_RETURN;
  }

  // In direct IO mode, blocks share the direct io buffer.
  // Otherwise, blocks share the scratch buffer.
  const bool use_shared_buffer = file->use_direct_io() || scratch != nullptr;

  autovector<FSReadRequest, MultiGetContext::MAX_BATCH_SIZE> read_reqs;
  size_t buf_offset = 0;
  size_t idx_in_batch = 0;

  uint64_t prev_offset = 0;
  size_t prev_len = 0;
  autovector<size_t, MultiGetContext::MAX_BATCH_SIZE> req_idx_for_block;
  autovector<size_t, MultiGetContext::MAX_BATCH_SIZE> req_offset_for_block;
  for (auto mget_iter = batch->begin(); mget_iter != batch->end();
       ++mget_iter, ++idx_in_batch) {
    const BlockHandle& handle = (*handles)[idx_in_batch];
    if (handle.IsNull()) {
      continue;
    }

    size_t prev_end = static_cast<size_t>(prev_offset) + prev_len;

    // If current block is adjacent to the previous one, at the same time,
    // compression is enabled and there is no compressed cache, we combine
    // the two block read as one.
    // We don't combine block reads here in direct IO mode, because when doing
    // direct IO read, the block requests will be realigned and merged when
    // necessary.
    if ((use_shared_buffer || use_fs_scratch) && !file->use_direct_io() &&
        prev_end == handle.offset()) {
      req_offset_for_block.emplace_back(prev_len);
      prev_len += BlockSizeWithTrailer(handle);
    } else {
      // No compression or current block and previous one is not adjacent:
      // Step 1, create a new request for previous blocks
      if (prev_len != 0) {
        FSReadRequest req;
        req.offset = prev_offset;
        req.len = prev_len;
        if (file->use_direct_io() || use_fs_scratch) {
          req.scratch = nullptr;
        } else if (use_shared_buffer) {
          req.scratch = scratch + buf_offset;
          buf_offset += req.len;
        } else {
          req.scratch = new char[req.len];
        }
        read_reqs.emplace_back(std::move(req));
      }

      // Step 2, remember the previous block info
      prev_offset = handle.offset();
      prev_len = BlockSizeWithTrailer(handle);
      req_offset_for_block.emplace_back(0);
    }
    req_idx_for_block.emplace_back(read_reqs.size());

    PERF_COUNTER_ADD(block_read_count, 1);
    PERF_COUNTER_ADD(block_read_byte, BlockSizeWithTrailer(handle));
  }
  // Handle the last block and process the pending last request
  if (prev_len != 0) {
    FSReadRequest req;
    req.offset = prev_offset;
    req.len = prev_len;
    if (file->use_direct_io() || use_fs_scratch) {
      req.scratch = nullptr;
    } else if (use_shared_buffer) {
      req.scratch = scratch + buf_offset;
    } else {
      req.scratch = new char[req.len];
    }
    read_reqs.emplace_back(std::move(req));
  }

  AlignedBuf direct_io_buf;
  {
    IOOptions opts;
    IODebugContext dbg;
    IOStatus s = file->PrepareIOOptions(options, opts, &dbg);
    if (s.ok()) {
#if defined(WITH_COROUTINES)
      if (file->use_direct_io()) {
#endif  // WITH_COROUTINES
        s = file->MultiRead(opts, &read_reqs[0], read_reqs.size(),
                            &direct_io_buf, &dbg);
#if defined(WITH_COROUTINES)
      } else {
        co_await batch->context()->reader().MultiReadAsync(
            file, opts, &read_reqs[0], read_reqs.size(), &direct_io_buf, &dbg);
      }
#endif  // WITH_COROUTINES
    }
    if (!s.ok()) {
      // Discard all the results in this batch if there is any time out
      // or overall MultiRead error
      for (FSReadRequest& req : read_reqs) {
        req.status = s;
      }
    }
  }

  idx_in_batch = 0;
  size_t valid_batch_idx = 0;
  for (auto mget_iter = batch->begin(); mget_iter != batch->end();
       ++mget_iter, ++idx_in_batch) {
    const BlockHandle& handle = (*handles)[idx_in_batch];

    if (handle.IsNull()) {
      continue;
    }

    assert(valid_batch_idx < req_idx_for_block.size());
    assert(valid_batch_idx < req_offset_for_block.size());
    assert(req_idx_for_block[valid_batch_idx] < read_reqs.size());
    size_t& req_idx = req_idx_for_block[valid_batch_idx];
    size_t& req_offset = req_offset_for_block[valid_batch_idx];
    valid_batch_idx++;
    FSReadRequest& req = read_reqs[req_idx];
    Status s = req.status;
    if (s.ok()) {
      if ((req.result.size() != req.len) ||
          (req_offset + BlockSizeWithTrailer(handle) > req.result.size())) {
        s = Status::Corruption("truncated block read from " +
                               rep_->file->file_name() + " offset " +
                               std::to_string(handle.offset()) + ", expected " +
                               std::to_string(req.len) + " bytes, got " +
                               std::to_string(req.result.size()));
      }
    }

    BlockContents serialized_block;
    if (s.ok()) {
      if (!use_fs_scratch && !use_shared_buffer) {
        // We allocated a buffer for this block. Give ownership of it to
        // BlockContents so it can free the memory
        assert(req.result.data() == req.scratch);
        assert(req.result.size() == BlockSizeWithTrailer(handle));
        assert(req_offset == 0);
        serialized_block =
            BlockContents(std::unique_ptr<char[]>(req.scratch), handle.size());
      } else {
        // We used the scratch buffer or direct io buffer
        // which are shared by the blocks.
        // In case of use_fs_scratch, underlying file system provided buffer is
        // used. serialized_block does not have the ownership.
        serialized_block =
            BlockContents(Slice(req.result.data() + req_offset, handle.size()));
      }
#ifndef NDEBUG
      serialized_block.has_trailer = true;
#endif

      if (options.verify_checksums) {
        PERF_TIMER_GUARD(block_checksum_time);
        const char* data = serialized_block.data.data();
        // Since the scratch might be shared, the offset of the data block in
        // the buffer might not be 0. req.result.data() only point to the
        // begin address of each read request, we need to add the offset
        // in each read request. Checksum is stored in the block trailer,
        // beyond the payload size.
        s = VerifyBlockChecksum(footer, data, handle.size(),
                                rep_->file->file_name(), handle.offset());
        RecordTick(ioptions.stats, BLOCK_CHECKSUM_COMPUTE_COUNT);
        if (!s.ok()) {
          RecordTick(ioptions.stats, BLOCK_CHECKSUM_MISMATCH_COUNT);
        }
        TEST_SYNC_POINT_CALLBACK("RetrieveMultipleBlocks:VerifyChecksum", &s);
        if (!s.ok() &&
            CheckFSFeatureSupport(ioptions.fs.get(),
                                  FSSupportedOps::kVerifyAndReconstructRead)) {
          assert(s.IsCorruption());
          assert(!ioptions.allow_mmap_reads);
          RecordTick(ioptions.stats, FILE_READ_CORRUPTION_RETRY_COUNT);

          // Repeat the read for this particular block using the regular
          // synchronous Read API. We can use the same chunk of memory
          // pointed to by data, since the size is identical and we know
          // its not a memory mapped file
          Slice result;
          IOOptions opts;
          IODebugContext dbg;
          IOStatus io_s = file->PrepareIOOptions(options, opts, &dbg);
          opts.verify_and_reconstruct_read = true;
          io_s = file->Read(opts, handle.offset(), BlockSizeWithTrailer(handle),
                            &result, const_cast<char*>(data), nullptr, &dbg);
          if (io_s.ok()) {
            assert(result.data() == data);
            assert(result.size() == BlockSizeWithTrailer(handle));
            s = VerifyBlockChecksum(footer, data, handle.size(),
                                    rep_->file->file_name(), handle.offset());
            if (s.ok()) {
              RecordTick(ioptions.stats,
                         FILE_READ_CORRUPTION_RETRY_SUCCESS_COUNT);
            }
          } else {
            s = io_s;
          }
        }
      }
    } else if (!use_shared_buffer) {
      // Free the allocated scratch buffer.
      delete[] req.scratch;
    }

    if (s.ok()) {
      s = CreateAndPinBlockInCache(options, handle, decomp, &serialized_block,
                                   &results[idx_in_batch]);
    }
    statuses[idx_in_batch] = s;
  }

  if (use_fs_scratch) {
    // Free the allocated scratch buffer by fs here as read requests might have
    // been combined into one.
    for (FSReadRequest& req : read_reqs) {
      if (req.fs_scratch != nullptr) {
        req.fs_scratch.reset();
        req.fs_scratch = nullptr;
      }
    }
  }
}

using MultiGetRange = MultiGetContext::Range;
DEFINE_SYNC_AND_ASYNC(void, BlockBasedTable::MultiGet)
(const ReadOptions& read_options, const MultiGetRange* mget_range,
 const SliceTransform* prefix_extractor, bool skip_filters) {
  if (mget_range->empty()) {
    // Caller should ensure non-empty (performance bug)
    assert(false);
    CO_RETURN;  // Nothing to do
  }

  FilterBlockReader* const filter =
      !skip_filters ? rep_->filter.get() : nullptr;
  MultiGetRange sst_file_range(*mget_range, mget_range->begin(),
                               mget_range->end());

  // First check the full filter
  // If full filter not useful, Then go into each block
  uint64_t tracing_mget_id = BlockCacheTraceHelper::kReservedGetId;
  if (sst_file_range.begin()->get_context) {
    tracing_mget_id = sst_file_range.begin()->get_context->get_tracing_get_id();
  }
  // TODO: need more than one lookup_context here to track individual filter
  // and index partition hits and misses.
  BlockCacheLookupContext metadata_lookup_context{
      TableReaderCaller::kUserMultiGet, tracing_mget_id,
      /*_get_from_user_specified_snapshot=*/read_options.snapshot != nullptr};
  FullFilterKeysMayMatch(filter, &sst_file_range, prefix_extractor,
                         &metadata_lookup_context, read_options);

  if (!sst_file_range.empty() && rep_->experimental_kvsep_bptree_enabled &&
      (rep_->kvsep_bptree_leaf_format_version == 2 ||
       rep_->kvsep_bptree_leaf_format_version == 3)) {
    const bool kvsep_leaf_v2 = rep_->kvsep_bptree_leaf_format_version == 2;
    const bool kvsep_pair_v3 = rep_->kvsep_bptree_leaf_format_version == 3;

    // Experimental KV-sep leaf (v2) / pair (v3) uses a custom per-leaf encoding,
    // so the existing MultiGet path (which assumes standard data blocks) is not
    // applicable.
    //
    // Fast path: group keys by leaf BlockHandle and reuse:
    //  - leaf block read + parse
    //  - per-leaf value-only block read
    //
    // This is designed to reduce redundant I/O and parsing when many keys fall
    // into the same leaf.

    if (!rep_->table_options.experimental_kvsep_bptree_multiget_leaf_grouping) {
      // Baseline / correctness-first path: fall back to per-key Get().
      for (auto miter = sst_file_range.begin(); miter != sst_file_range.end();
           ++miter) {
        if (miter->s == nullptr) {
          continue;
        }
        *(miter->s) = Status::OK();
        if (miter->get_context == nullptr) {
          continue;
        }
        // FullFilterKeysMayMatch() already ran above.
        *(miter->s) =
            Get(read_options, miter->ikey, miter->get_context, prefix_extractor,
                /*skip_filters=*/true);
      }
      CO_RETURN;
    }

    IndexBlockIter iiter_on_stack;
    bool need_upper_bound_check = false;
    if (rep_->index_type == BlockBasedTableOptions::kHashSearch) {
      need_upper_bound_check = PrefixExtractorChanged(prefix_extractor);
    }
    auto iiter = NewIndexIterator(
        read_options, need_upper_bound_check, &iiter_on_stack,
        sst_file_range.begin()->get_context, &metadata_lookup_context);
    std::unique_ptr<InternalIteratorBase<IndexValue>> iiter_unique_ptr;
    if (iiter != &iiter_on_stack) {
      iiter_unique_ptr.reset(iiter);
    }

    struct LeafGroup {
      BlockHandle leaf;
      autovector<KeyContext*, MultiGetContext::MAX_BATCH_SIZE> keys;
    };
    autovector<LeafGroup, MultiGetContext::MAX_BATCH_SIZE> groups;

    auto add_to_group = [&](const BlockHandle& leaf, KeyContext* kc) {
      for (auto& g : groups) {
        if (g.leaf.offset() == leaf.offset() && g.leaf.size() == leaf.size()) {
          g.keys.push_back(kc);
          return;
        }
      }
      LeafGroup ng;
      ng.leaf = leaf;
      ng.keys.push_back(kc);
      groups.push_back(std::move(ng));
    };

    // 1) Locate leaf handle for each key via the KV-sep B+tree index.
    for (auto miter = sst_file_range.begin(); miter != sst_file_range.end();
         ++miter) {
      if (miter->s == nullptr) {
        continue;
      }
      // Default to OK (not found is not an error).
      *(miter->s) = Status::OK();
      if (miter->get_context == nullptr) {
        continue;
      }

      iiter->Seek(miter->ikey);
      if (!iiter->status().ok()) {
        *(miter->s) = iiter->status();
        continue;
      }
      if (!iiter->Valid()) {
        continue;
      }
      if (UNLIKELY(iiter->value().handle.IsNull())) {
        *(miter->s) = Status::Corruption(
            kvsep_pair_v3 ? "kvsep pair v3: null pair handle"
                          : "kvsep leaf v2: null leaf handle");
        continue;
      }
      if (UNLIKELY(iiter->value().handle.offset() + iiter->value().handle.size() +
                       BlockBasedTable::kBlockTrailerSize >
                   rep_->file_size)) {
        *(miter->s) = Status::Corruption(
            kvsep_pair_v3 ? "kvsep pair v3: pair handle out of file range"
                          : "kvsep leaf v2: leaf handle out of file range");
        continue;
      }
      add_to_group(iiter->value().handle, &(*miter));
    }

    uint64_t grouped_keys = 0;
    for (const auto& g : groups) {
      grouped_keys += g.keys.size();
    }
    RecordTick(rep_->ioptions.stats, EXPERIMENTAL_KVSEP_BPTREE_MGET_KEYS_GROUPED,
               grouped_keys);
    RecordTick(rep_->ioptions.stats, EXPERIMENTAL_KVSEP_BPTREE_MGET_LEAF_GROUPS,
               groups.size());
    if (grouped_keys >= groups.size()) {
      RecordTick(rep_->ioptions.stats,
                 EXPERIMENTAL_KVSEP_BPTREE_MGET_LEAF_REUSE_KEYS,
                 grouped_keys - groups.size());
    }

    // 2) Sort groups by on-disk offset for better locality (small N).
    std::sort(groups.begin(), groups.end(),
              [](const LeafGroup& a, const LeafGroup& b) {
                return a.leaf.offset() < b.leaf.offset();
              });

    // 2.5) Use a shared prefetch buffer across groups. Since we sort by offset,
    // these reads are usually sequential and can benefit from readahead.
    std::unique_ptr<FilePrefetchBuffer> kvsep_prefetch_buffer;
    CreateExperimentalKVSepPrefetchBufferIfNeeded(
        rep_, read_options, &kvsep_prefetch_buffer,
        /*fallback_readahead_bytes=*/256 * 1024, FilePrefetchBufferUsage::kUnknown);
    FilePrefetchBuffer* const kvsep_fpb = kvsep_prefetch_buffer.get();

    // 3) Process each leaf group.
    for (auto& g : groups) {
      RecordTick(rep_->ioptions.stats, EXPERIMENTAL_KVSEP_BPTREE_MGET_LEAF_BLOCKS,
                 1);
      BlockCacheLookupContext leaf_lookup_context{
          TableReaderCaller::kUserMultiGet, tracing_mget_id,
          /*get_from_user_specified_snapshot=*/read_options.snapshot != nullptr};

      if (kvsep_leaf_v2) {
        CachableEntry<Block_kKVSepLeaf> leaf_block;
        Status leaf_status = RetrieveBlock<Block_kKVSepLeaf>(
            /*prefetch_buffer=*/kvsep_fpb, read_options, g.leaf,
            rep_->decompressor.get(), &leaf_block,
            /*get_context=*/g.keys.front()->get_context, &leaf_lookup_context,
            /*for_compaction=*/false, /*use_cache=*/read_options.fill_cache,
            /*async_read=*/false,
            /*use_block_cache_for_lookup=*/true);
        if (kvsep_fpb) {
          kvsep_fpb->UpdateReadPattern(g.leaf.offset(),
                                      BlockBasedTable::BlockSizeWithTrailer(g.leaf),
                                      read_options.adaptive_readahead);
        }

        if (read_options.read_tier == kBlockCacheTier &&
            leaf_status.IsIncomplete()) {
          for (auto* kc : g.keys) {
            if (kc->get_context) {
              kc->get_context->MarkKeyMayExist();
            }
            if (kc->s) {
              *(kc->s) = leaf_status;
            }
          }
          continue;
        }
        if (!leaf_status.ok()) {
          for (auto* kc : g.keys) {
            if (kc->s) {
              *(kc->s) = leaf_status;
            }
          }
          continue;
        }
        if (UNLIKELY(leaf_block.GetValue() == nullptr)) {
          Status s = Status::Corruption("kvsep leaf v2: missing leaf block");
          for (auto* kc : g.keys) {
            if (kc->s) {
              *(kc->s) = s;
            }
          }
          continue;
        }
        if (UNLIKELY(leaf_block.GetValue()->ContentSlice().size() < 4)) {
          Status s = Status::Corruption(
              "kvsep leaf v2: leaf block too small: off=" +
              std::to_string(g.leaf.offset()) +
              " size=" + std::to_string(g.leaf.size()) +
              " block_bytes=" +
              std::to_string(leaf_block.GetValue()->ContentSlice().size()));
          for (auto* kc : g.keys) {
            if (kc->s) {
              *(kc->s) = s;
            }
          }
          continue;
        }

        KVSepBptreeLeafV2View leaf_view;
        Status parse_s =
            leaf_view.InitFromContents(leaf_block.GetValue()->ContentSlice());
        if (UNLIKELY(!parse_s.ok())) {
          for (auto* kc : g.keys) {
            if (kc->s) {
              *(kc->s) = parse_s;
            }
          }
          continue;
        }

        // Load the per-leaf value-only block once.
        const BlockHandle vb_handle = leaf_view.value_block_handle();
        CachableEntry<Block_kKVSepValue> kvsep_value_block;
        Status vb_status = Status::OK();
        if (!vb_handle.IsNull()) {
          RecordTick(rep_->ioptions.stats,
                     EXPERIMENTAL_KVSEP_BPTREE_MGET_VALUE_BLOCKS, 1);
          vb_status = KVSepBptreeGetValueBlock(read_options, vb_handle,
                                               &kvsep_value_block,
                                               &leaf_lookup_context, kvsep_fpb);
        }
        if (UNLIKELY(!vb_status.ok())) {
          for (auto* kc : g.keys) {
            if (kc->s) {
              *(kc->s) = vb_status;
            }
          }
          continue;
        }
        const Slice vb_contents =
            kvsep_value_block.GetValue()
                ? kvsep_value_block.GetValue()->ContentSlice()
                : Slice();

        // Helper: lower_bound in leaf by internal key comparator.
        auto lower_bound =
            [&](const Slice& target, std::string* scratch) -> uint32_t {
          const size_t prefix_len = leaf_view.prefix().size();
          uint32_t left = 0;
          uint32_t right = leaf_view.num_entries();
          while (left < right) {
            const uint32_t mid = left + (right - left) / 2;
            const Slice mid_key = (prefix_len == 0)
                                      ? leaf_view.SuffixAt(mid)
                                      : leaf_view.FullKeyAtWithScratchPrefix(
                                            mid, scratch, prefix_len);
            const int cmp = rep_->internal_comparator.Compare(mid_key, target);
            if (cmp < 0) {
              left = mid + 1;
            } else {
              right = mid;
            }
          }
          return left;
        };

        UserComparatorWrapper ucmp(rep_->internal_comparator.user_comparator());

        // 4) Resolve each key within this leaf.
        std::string entry_key_scratch;
        entry_key_scratch.reserve(64);
        const size_t leaf_prefix_len = leaf_view.prefix().size();
        if (leaf_prefix_len != 0) {
          entry_key_scratch.assign(leaf_view.prefix().data(), leaf_prefix_len);
        }
        for (auto* kc : g.keys) {
          if (kc == nullptr || kc->s == nullptr) {
            continue;
          }
          // If an earlier stage already set an error, keep it.
          if (!kc->s->ok()) {
            continue;
          }
          if (kc->get_context == nullptr) {
            continue;
          }

          const Slice& ikey = kc->ikey;
          const Slice target_user_key = ExtractUserKey(ikey);
          bool matched = false;
          bool done = false;
          if (leaf_prefix_len != 0) {
            entry_key_scratch.resize(leaf_prefix_len);
          } else {
            entry_key_scratch.clear();
          }
          const uint32_t pos = lower_bound(ikey, &entry_key_scratch);
          for (uint32_t i = pos; i < leaf_view.num_entries() && !done; ++i) {
            const Slice entry_key =
                (leaf_prefix_len == 0)
                    ? leaf_view.SuffixAt(i)
                    : leaf_view.FullKeyAtWithScratchPrefix(i, &entry_key_scratch,
                                                          leaf_prefix_len);

            // Stop once we've moved past this user key.
            if (ucmp.CompareWithoutTimestamp(ExtractUserKey(entry_key),
                                             target_user_key) > 0) {
              break;
            }

            ParsedInternalKey parsed_key;
            Status pik_status =
                ParseInternalKey(entry_key, &parsed_key, false /* log_err_key */);
            if (UNLIKELY(!pik_status.ok())) {
              *(kc->s) = pik_status;
              done = true;
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
                           vb_contents.size())) {
                *(kc->s) = Status::Corruption(
                    "kvsep leaf v2: value pointer out of range");
                done = true;
                break;
              }
              value_to_save = Slice(vb_contents.data() + value_off, value_len);
            }

            Status read_status;
            bool ret = kc->get_context->SaveValue(
                parsed_key, value_to_save, &matched, &read_status,
                /*value_pinner=*/nullptr);
            if (UNLIKELY(!read_status.ok())) {
              *(kc->s) = read_status;
              done = true;
              break;
            }
            if (!ret) {
              done = true;
              break;
            }
          }

          // If SaveValue indicates we still need more operands (e.g. merge) and
          // they spilled into the next leaf, fall back to Get() for correctness.
          if (kc->s->ok() && !done &&
              kc->get_context->State() == GetContext::GetState::kMerge) {
            RecordTick(rep_->ioptions.stats,
                       EXPERIMENTAL_KVSEP_BPTREE_MGET_FALLBACK_GET, 1);
            *(kc->s) =
                Get(read_options, kc->ikey, kc->get_context, prefix_extractor,
                    /*skip_filters=*/true);
          }
        }
      } else {
        assert(kvsep_pair_v3);

        CachableEntry<Block_kKVSepPair> pair_block;
        Status pair_status = RetrieveBlock<Block_kKVSepPair>(
            /*prefetch_buffer=*/kvsep_fpb, read_options, g.leaf,
            rep_->decompressor.get(), &pair_block,
            /*get_context=*/g.keys.front()->get_context, &leaf_lookup_context,
            /*for_compaction=*/false, /*use_cache=*/read_options.fill_cache,
            /*async_read=*/false,
            /*use_block_cache_for_lookup=*/true);
        if (kvsep_fpb) {
          kvsep_fpb->UpdateReadPattern(g.leaf.offset(),
                                      BlockBasedTable::BlockSizeWithTrailer(g.leaf),
                                      read_options.adaptive_readahead);
        }
        if (pair_status.ok() && !pair_block.IsCached()) {
          const uint64_t bytes = BlockBasedTable::BlockSizeWithTrailer(g.leaf);
          RecordTick(rep_->ioptions.stats,
                     EXPERIMENTAL_KVSEP_BPTREE_PAIR_FILE_READS_MULTIGET, 1);
          RecordTick(
              rep_->ioptions.stats,
              EXPERIMENTAL_KVSEP_BPTREE_PAIR_FILE_READ_BYTES_MULTIGET, bytes);
        }

        if (read_options.read_tier == kBlockCacheTier &&
            pair_status.IsIncomplete()) {
          for (auto* kc : g.keys) {
            if (kc->get_context) {
              kc->get_context->MarkKeyMayExist();
            }
            if (kc->s) {
              *(kc->s) = pair_status;
            }
          }
          continue;
        }
        if (!pair_status.ok()) {
          for (auto* kc : g.keys) {
            if (kc->s) {
              *(kc->s) = pair_status;
            }
          }
          continue;
        }
        if (UNLIKELY(pair_block.GetValue() == nullptr)) {
          Status s = Status::Corruption("kvsep pair v3: missing pair block");
          for (auto* kc : g.keys) {
            if (kc->s) {
              *(kc->s) = s;
            }
          }
          continue;
        }

        KVSepBptreePairV3View pair_view;
        Status parse_pair =
            pair_view.InitFromContents(pair_block.GetValue()->ContentSlice());
        if (UNLIKELY(!parse_pair.ok())) {
          for (auto* kc : g.keys) {
            if (kc->s) {
              *(kc->s) = parse_pair;
            }
          }
          continue;
        }
        KVSepBptreeLeafV3View leaf_view;
        Status parse_leaf = leaf_view.InitFromContents(pair_view.leaf_contents());
        if (UNLIKELY(!parse_leaf.ok())) {
          for (auto* kc : g.keys) {
            if (kc->s) {
              *(kc->s) = parse_leaf;
            }
          }
          continue;
        }

        const Slice vb_contents = pair_view.value_contents();

        auto lower_bound =
            [&](const Slice& target, std::string* scratch) -> uint32_t {
          const size_t prefix_len = leaf_view.prefix().size();
          uint32_t left = 0;
          uint32_t right = leaf_view.num_entries();
          while (left < right) {
            const uint32_t mid = left + (right - left) / 2;
            const Slice mid_key = (prefix_len == 0)
                                      ? leaf_view.SuffixAt(mid)
                                      : leaf_view.FullKeyAtWithScratchPrefix(
                                            mid, scratch, prefix_len);
            const int cmp = rep_->internal_comparator.Compare(mid_key, target);
            if (cmp < 0) {
              left = mid + 1;
            } else {
              right = mid;
            }
          }
          return left;
        };

        UserComparatorWrapper ucmp(rep_->internal_comparator.user_comparator());

        std::string entry_key_scratch;
        entry_key_scratch.reserve(64);
        const size_t leaf_prefix_len = leaf_view.prefix().size();
        if (leaf_prefix_len != 0) {
          entry_key_scratch.assign(leaf_view.prefix().data(), leaf_prefix_len);
        }
        for (auto* kc : g.keys) {
          if (kc == nullptr || kc->s == nullptr) {
            continue;
          }
          if (!kc->s->ok()) {
            continue;
          }
          if (kc->get_context == nullptr) {
            continue;
          }

          const Slice& ikey = kc->ikey;
          const Slice target_user_key = ExtractUserKey(ikey);
          bool matched = false;
          bool done = false;
          if (leaf_prefix_len != 0) {
            entry_key_scratch.resize(leaf_prefix_len);
          } else {
            entry_key_scratch.clear();
          }
          const uint32_t pos = lower_bound(ikey, &entry_key_scratch);
          for (uint32_t i = pos; i < leaf_view.num_entries() && !done; ++i) {
            const Slice entry_key =
                (leaf_prefix_len == 0)
                    ? leaf_view.SuffixAt(i)
                    : leaf_view.FullKeyAtWithScratchPrefix(i, &entry_key_scratch,
                                                          leaf_prefix_len);
            if (ucmp.CompareWithoutTimestamp(ExtractUserKey(entry_key),
                                             target_user_key) > 0) {
              break;
            }

            ParsedInternalKey parsed_key;
            Status pik_status =
                ParseInternalKey(entry_key, &parsed_key, false /* log_err_key */);
            if (UNLIKELY(!pik_status.ok())) {
              *(kc->s) = pik_status;
              done = true;
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
                           vb_contents.size())) {
                *(kc->s) =
                    Status::Corruption("kvsep pair v3: value pointer out of range");
                done = true;
                break;
              }
              value_to_save = Slice(vb_contents.data() + value_off, value_len);
            }

            Status read_status;
            bool ret = kc->get_context->SaveValue(
                parsed_key, value_to_save, &matched, &read_status,
                /*value_pinner=*/nullptr);
            if (UNLIKELY(!read_status.ok())) {
              *(kc->s) = read_status;
              done = true;
              break;
            }
            if (!ret) {
              done = true;
              break;
            }
          }

          if (kc->s->ok() && !done &&
              kc->get_context->State() == GetContext::GetState::kMerge) {
            RecordTick(rep_->ioptions.stats,
                       EXPERIMENTAL_KVSEP_BPTREE_MGET_FALLBACK_GET, 1);
            *(kc->s) =
                Get(read_options, kc->ikey, kc->get_context, prefix_extractor,
                    /*skip_filters=*/true);
          }
        }
      }
    }

    CO_RETURN;
  }

  if (!sst_file_range.empty()) {
    IndexBlockIter iiter_on_stack;
    // if prefix_extractor found in block differs from options, disable
    // BlockPrefixIndex. Only do this check when index_type is kHashSearch.
    bool need_upper_bound_check = false;
    if (rep_->index_type == BlockBasedTableOptions::kHashSearch) {
      need_upper_bound_check = PrefixExtractorChanged(prefix_extractor);
    }
    auto iiter = NewIndexIterator(
        read_options, need_upper_bound_check, &iiter_on_stack,
        sst_file_range.begin()->get_context, &metadata_lookup_context);
    std::unique_ptr<InternalIteratorBase<IndexValue>> iiter_unique_ptr;
    if (iiter != &iiter_on_stack) {
      iiter_unique_ptr.reset(iiter);
    }

	    uint64_t prev_offset = std::numeric_limits<uint64_t>::max();
	    autovector<BlockHandle, MultiGetContext::MAX_BATCH_SIZE> block_handles;
	    // NOTE: `block_handles` is mutated as part of MultiGet execution:
	    // - Set to NullBlockHandle() for keys that reuse a prior key's data block
	    // - Set to NullBlockHandle() on block cache hits (so later we don't issue IO)
	    //
	    // Some consumers (e.g. experimental kv-separation) still need to know the
	    // data block's BlockHandle (offset/size) even when the block was served
	    // from cache. Preserve the original handles here.
	    std::vector<BlockHandle> original_block_handles;
	    std::array<CachableEntry<Block_kData>, MultiGetContext::MAX_BATCH_SIZE>
	        results;
	    std::array<Status, MultiGetContext::MAX_BATCH_SIZE> statuses;
    // Empty data_lookup_contexts means "unused," when block cache tracing is
    // disabled. (Limited options as element type is not default contructible.)
    std::vector<BlockCacheLookupContext> data_lookup_contexts;
    MultiGetContext::Mask reused_mask = 0;
    char stack_buf[kMultiGetReadStackBufSize];
    std::unique_ptr<char[]> block_buf;
    if (block_cache_tracer_ && block_cache_tracer_->is_tracing_enabled()) {
      // Awkward because BlockCacheLookupContext is not CopyAssignable
      data_lookup_contexts.reserve(MultiGetContext::MAX_BATCH_SIZE);
      for (size_t i = 0; i < MultiGetContext::MAX_BATCH_SIZE; ++i) {
        data_lookup_contexts.push_back(metadata_lookup_context);
      }
    }
    {
      MultiGetRange data_block_range(sst_file_range, sst_file_range.begin(),
                                     sst_file_range.end());
      CachableEntry<DecompressorDict> dict;
      Status dict_status;
      dict_status.PermitUncheckedError();
      bool dict_inited = false;
      size_t total_len = 0;

      // GetContext for any key will do, as the stats will be aggregated
      // anyway
      GetContext* get_context = sst_file_range.begin()->get_context;

      {
        using BCI = BlockCacheInterface<Block_kData>;
        BCI block_cache{rep_->table_options.block_cache.get()};
        std::array<BCI::TypedAsyncLookupHandle, MultiGetContext::MAX_BATCH_SIZE>
            async_handles;
        BlockCreateContext create_ctx = rep_->create_context;
        std::array<CacheKey, MultiGetContext::MAX_BATCH_SIZE> cache_keys;
        size_t cache_lookup_count = 0;

        for (auto miter = data_block_range.begin();
             miter != data_block_range.end(); ++miter) {
          const Slice& key = miter->ikey;
          iiter->Seek(miter->ikey);

          IndexValue v;
          if (iiter->Valid()) {
            v = iiter->value();
          }
          if (!iiter->Valid() ||
              (!v.first_internal_key.empty() && !skip_filters &&
               UserComparatorWrapper(
                   rep_->internal_comparator.user_comparator())
                       .CompareWithoutTimestamp(
                           ExtractUserKey(key),
                           ExtractUserKey(v.first_internal_key)) < 0)) {
            // The requested key falls between highest key in previous block and
            // lowest key in current block.
            if (!iiter->status().IsNotFound()) {
              *(miter->s) = iiter->status();
            }
            data_block_range.SkipKey(miter);
            sst_file_range.SkipKey(miter);
            continue;
          }

          if (!dict_inited && rep_->uncompression_dict_reader) {
            dict_status = rep_->uncompression_dict_reader
                              ->GetOrReadUncompressionDictionary(
                                  nullptr /* prefetch_buffer */, read_options,
                                  get_context, &metadata_lookup_context, &dict);
            dict_inited = true;
          }

          if (!dict_status.ok()) {
            assert(!dict_status.IsNotFound());
            *(miter->s) = dict_status;
            data_block_range.SkipKey(miter);
            sst_file_range.SkipKey(miter);
            continue;
          } else {
            assert(!dict_inited || dict.GetValue() != nullptr);
          }
          if (dict.GetValue()) {
            create_ctx.decompressor = dict.GetValue()->decompressor_.get();
          }

          if (v.handle.offset() == prev_offset) {
            // This key can reuse the previous block (later on).
            // Mark previous as "reused"
            reused_mask |= MultiGetContext::Mask{1}
                           << (block_handles.size() - 1);
            // Use null handle to indicate this one reuses same block as
            // previous.
            block_handles.emplace_back(BlockHandle::NullBlockHandle());
            continue;
          }
          prev_offset = v.handle.offset();
          block_handles.emplace_back(v.handle);

          if (block_cache) {
            // Lookup the cache for the given data block referenced by an index
            // iterator value (i.e BlockHandle). If it exists in the cache,
            // initialize block to the contents of the data block.

            // An async version of MaybeReadBlockAndLoadToCache /
            // GetDataBlockFromCache
            BCI::TypedAsyncLookupHandle& async_handle =
                async_handles[cache_lookup_count];
            cache_keys[cache_lookup_count] =
                GetCacheKey(rep_->base_cache_key, v.handle);
            async_handle.key = cache_keys[cache_lookup_count].AsSlice();
            // NB: StartAsyncLookupFull populates async_handle.helper
            async_handle.create_context = &create_ctx;
            async_handle.priority = GetCachePriority<Block_kData>();
            async_handle.stats = rep_->ioptions.statistics.get();

            block_cache.StartAsyncLookupFull(
                async_handle, rep_->ioptions.lowest_used_cache_tier);
            ++cache_lookup_count;
            // TODO: stats?
          }
        }

	        if (block_cache) {
	          block_cache.get()->WaitAll(&async_handles[0], cache_lookup_count);
	        }
	        original_block_handles.assign(block_handles.begin(), block_handles.end());
	        size_t lookup_idx = 0;
	        for (size_t i = 0; i < block_handles.size(); ++i) {
	          // If this block was a success or failure or not needed because
	          // the corresponding key is in the same block as a prior key, skip
          if (block_handles[i] == BlockHandle::NullBlockHandle()) {
            continue;
          }
          if (!block_cache) {
            total_len += BlockSizeWithTrailer(block_handles[i]);
          } else {
            BCI::TypedHandle* h = async_handles[lookup_idx].Result();
            if (h) {
              // Cache hit
              results[i].SetCachedValue(block_cache.Value(h), block_cache.get(),
                                        h);
              // Don't need to fetch
              block_handles[i] = BlockHandle::NullBlockHandle();
              UpdateCacheHitMetrics(BlockType::kData, get_context,
                                    block_cache.get()->GetUsage(h));
            } else {
              // Cache miss
              total_len += BlockSizeWithTrailer(block_handles[i]);
              UpdateCacheMissMetrics(BlockType::kData, get_context);
            }
            if (!data_lookup_contexts.empty()) {
              // Populate cache key before it's discarded
              data_lookup_contexts[i].block_key =
                  async_handles[lookup_idx].key.ToString();
            }
            ++lookup_idx;
          }
        }
        assert(lookup_idx == cache_lookup_count);
      }

      if (total_len) {
        char* scratch = nullptr;
        bool use_fs_scratch = false;
        assert(dict_inited || !rep_->uncompression_dict_reader);
        assert(dict_status.ok());

        if (!rep_->file->use_direct_io()) {
          if (CheckFSFeatureSupport(rep_->ioptions.fs.get(),
                                    FSSupportedOps::kFSBuffer)) {
            use_fs_scratch = true;
          }
        }

        // If using direct IO, then scratch is not used, so keep it nullptr.
        // If the blocks need to be uncompressed and we don't need the
        // compressed blocks, then we can use a contiguous block of
        // memory to read in all the blocks as it will be temporary
        // storage
        // 1. If blocks are compressed and compressed block cache is there,
        //    alloc heap bufs
        // 2. If blocks are uncompressed, alloc heap bufs
        // 3. If blocks are compressed and no compressed block cache, use
        //    stack buf
        if (!use_fs_scratch && !rep_->file->use_direct_io() &&
            rep_->decompressor) {
          if (total_len <= kMultiGetReadStackBufSize) {
            scratch = stack_buf;
          } else {
            scratch = new char[total_len];
            block_buf.reset(scratch);
          }
        }
        CO_AWAIT(RetrieveMultipleBlocks)
        (read_options, &data_block_range, &block_handles, &statuses[0],
         &results[0], scratch,
         dict.GetValue() ? dict.GetValue()->decompressor_.get()
                         : rep_->decompressor.get(),
         use_fs_scratch);
        if (get_context) {
          ++(get_context->get_context_stats_.num_sst_read);
        }
      }
    }

    DataBlockIter first_biter;
    DataBlockIter next_biter;
    size_t idx_in_batch = 0;
    SharedCleanablePtr shared_cleanable;
    BlockHandle prev_data_block_handle = BlockHandle::NullBlockHandle();
    for (auto miter = sst_file_range.begin(); miter != sst_file_range.end();
         ++miter) {
      Status s;
      GetContext* get_context = miter->get_context;
      const Slice& key = miter->ikey;
      bool matched = false;  // if such user key matched a key in SST
      bool done = false;
      bool first_block = true;
	      do {
	        DataBlockIter* biter = nullptr;
	        BlockHandle cur_data_block_handle = BlockHandle::NullBlockHandle();
	        uint64_t referenced_data_size = 0;
	        Block_kData* parsed_block_value = nullptr;
        bool reusing_prev_block;
        bool later_reused;
        bool does_referenced_key_exist = false;
        bool handle_present = false;
        BlockCacheLookupContext* lookup_data_block_context =
            data_lookup_contexts.empty() ? nullptr
                                         : &data_lookup_contexts[idx_in_batch];
	        if (first_block) {
	          handle_present = !block_handles[idx_in_batch].IsNull();
	          parsed_block_value = results[idx_in_batch].GetValue();
	          if (handle_present || parsed_block_value) {
	            // If the block was found in the block cache, `block_handles` is
	            // set to NullBlockHandle() to avoid reading the block from file,
	            // but we might still need the real handle (e.g. kvsep value map
	            // lookup keyed by data block offset).
	            if (cur_data_block_handle.IsNull()) {
	              cur_data_block_handle = block_handles[idx_in_batch];
	              if (cur_data_block_handle.IsNull() && parsed_block_value &&
	                  idx_in_batch < original_block_handles.size()) {
	                cur_data_block_handle = original_block_handles[idx_in_batch];
	              }
	            }
	            first_biter.Invalidate(Status::OK());
	            NewDataBlockIterator<DataBlockIter>(
	                read_options, results[idx_in_batch].As<Block>(), &first_biter,
	                statuses[idx_in_batch]);
	            reusing_prev_block = false;
	            if (!cur_data_block_handle.IsNull()) {
	              prev_data_block_handle = cur_data_block_handle;
	            }
	          } else {
	            // If handle is null and result is empty, then the status is never
	            // set, which should be the initial value: ok().
	            assert(statuses[idx_in_batch].ok());
            reusing_prev_block = true;
            cur_data_block_handle = prev_data_block_handle;
          }
          biter = &first_biter;
          later_reused =
              (reused_mask & (MultiGetContext::Mask{1} << idx_in_batch)) != 0;
          idx_in_batch++;
        } else {
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

          next_biter.Invalidate(Status::OK());
          Status tmp_s;
          NewDataBlockIterator<DataBlockIter>(
              read_options, iiter->value().handle, &next_biter,
              BlockType::kData, get_context, lookup_data_block_context,
              /* prefetch_buffer= */ nullptr, /* for_compaction = */ false,
              /*async_read = */ false, tmp_s,
              /* use_block_cache_for_lookup = */ true);
          biter = &next_biter;
          cur_data_block_handle = iiter->value().handle;
          reusing_prev_block = false;
          later_reused = false;
        }

        if (read_options.read_tier == kBlockCacheTier &&
            biter->status().IsIncomplete()) {
          // couldn't get block from block_cache
          // Update Saver.state to Found because we are only looking for
          // whether we can guarantee the key is not there with kBlockCacheTier
          get_context->MarkKeyMayExist();
          break;
        }
        if (!biter->status().ok()) {
          s = biter->status();
          break;
        }

        // Reusing blocks complicates pinning/Cleanable, because the cache
        // entry referenced by biter can only be released once all returned
        // pinned values are released. This code previously did an extra
        // block_cache Ref for each reuse, but that unnecessarily increases
        // block cache contention. Instead we can use a variant of shared_ptr
        // to release in block cache only once.
        //
        // Although the biter loop below might SaveValue multiple times for
        // merges, just one value_pinner suffices, as MultiGet will merge
        // the operands before returning to the API user.
        Cleanable* value_pinner;
        if (biter->IsValuePinned()) {
          if (reusing_prev_block) {
            // Note that we don't yet know if the MultiGet results will need
            // to pin this block, so we might wrap a block for sharing and
            // still end up with 1 (or 0) pinning ref. Not ideal but OK.
            //
            // Here we avoid adding redundant cleanups if we didn't end up
            // delegating the cleanup from last time around.
            if (!biter->HasCleanups()) {
              assert(shared_cleanable.get());
              if (later_reused) {
                shared_cleanable.RegisterCopyWith(biter);
              } else {
                shared_cleanable.MoveAsCleanupTo(biter);
              }
            }
          } else if (later_reused) {
            assert(biter->HasCleanups());
            // Make the existing cleanups on `biter` sharable:
            shared_cleanable.Allocate();
            // Move existing `biter` cleanup(s) to `shared_cleanable`
            biter->DelegateCleanupsTo(&*shared_cleanable);
            // Reference `shared_cleanable` as new cleanup for `biter`
            shared_cleanable.RegisterCopyWith(biter);
          }
          assert(biter->HasCleanups());
          value_pinner = biter;
        } else {
          value_pinner = nullptr;
        }

        const bool kvsep_enabled = rep_->experimental_kvsep_bptree_enabled;
        CachableEntry<Block_kKVSepValue> kvsep_value_block;
        bool kvsep_value_block_loaded = false;
        BlockHandle kvsep_value_block_loaded_handle =
            BlockHandle::NullBlockHandle();

        bool may_exist = biter->SeekForGet(key);
        if (!may_exist) {
          // HashSeek cannot find the key this block and the the iter is not
          // the end of the block, i.e. cannot be in the following blocks
          // either. In this case, the seek_key cannot be found, so we break
          // from the top level for-loop.
          break;
        }

        // Call the *saver function on each entry/block until it returns false
        for (; biter->status().ok() && biter->Valid(); biter->Next()) {
          ParsedInternalKey parsed_key;
          Status pik_status = ParseInternalKey(
              biter->key(), &parsed_key, false /* log_err_key */);  // TODO
          if (!pik_status.ok()) {
            s = pik_status;
            break;
          }
          Slice value_to_save = biter->value();
          if (kvsep_enabled) {
            BlockHandle kvsep_value_block_handle;
            uint32_t value_off = 0;
            uint32_t value_len = 0;
            Status decode_status = KVSepBptreeDecodePointer(value_to_save,
                                                           &kvsep_value_block_handle,
                                                           &value_off, &value_len);
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
                    lookup_data_block_context);
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
              (kvsep_enabled || !value_pinner) ? nullptr : value_pinner);
          if (!read_status.ok()) {
            s = read_status;
            break;
          }
          if (!ret) {
            if (get_context->State() == GetContext::GetState::kFound) {
              does_referenced_key_exist = true;
              referenced_data_size =
                  biter->key().size() + value_to_save.size();
            }
            done = true;
            break;
          }
        }
        // Write the block cache access.
        // XXX: There appear to be 'break' statements above that bypass this
        // writing of the block cache trace record
        if (lookup_data_block_context && !reusing_prev_block && first_block) {
          Slice referenced_key;
          if (does_referenced_key_exist) {
            referenced_key = biter->key();
          } else {
            referenced_key = key;
          }

          // block_key is self-assigned here (previously assigned from
          // cache_keys / async_handles, now out of scope)
          SaveLookupContextOrTraceRecord(lookup_data_block_context->block_key,
                                         /*is_cache_hit=*/!handle_present,
                                         read_options, parsed_block_value,
                                         lookup_data_block_context);
          FinishTraceRecord(
              *lookup_data_block_context, lookup_data_block_context->block_key,
              referenced_key, does_referenced_key_exist, referenced_data_size);
        }
        if (s.ok()) {
          s = biter->status();
        }
        if (done || !s.ok()) {
          // Avoid the extra Next which is expensive in two-level indexes
          break;
        }
        if (first_block) {
          iiter->Seek(key);
          if (!iiter->Valid()) {
            break;
          }
        }
        first_block = false;
        iiter->Next();
      } while (iiter->Valid());

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
      *(miter->s) = s;
    }
#ifdef ROCKSDB_ASSERT_STATUS_CHECKED
    // Not sure why we need to do it. Should investigate more.
    for (auto& st : statuses) {
      st.PermitUncheckedError();
    }
#endif  // ROCKSDB_ASSERT_STATUS_CHECKED
  }
}
}  // namespace ROCKSDB_NAMESPACE
#endif
