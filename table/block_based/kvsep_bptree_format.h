// This file contains experimental SST format helpers for KV-separation +
// B+Tree-like indexing experiments. It is not part of RocksDB stable ABI.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rocksdb/comparator.h"
#include "rocksdb/rocksdb_namespace.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "table/format.h"
#include "util/coding.h"

namespace ROCKSDB_NAMESPACE {

inline bool KVSepBptreeIsBytewiseComparator(const Comparator* ucmp) {
  return ucmp != nullptr && ucmp == BytewiseComparator();
}

// Helper: decode the last 8 bytes (seqno+type footer) of an internal key stored
// as `prefix || suffix`, without materializing a contiguous buffer.
//
// Returns false if the concatenated key is too small.
inline bool KVSepBptreeDecodeInternalKeyFooter(const Slice& prefix,
                                               const Slice& suffix,
                                               uint64_t* footer_out) {
  if (footer_out == nullptr) {
    return false;
  }
  const size_t total = prefix.size() + suffix.size();
  if (total < 8) {
    return false;
  }
  char tmp[8];
  if (suffix.size() >= 8) {
    memcpy(tmp, suffix.data() + (suffix.size() - 8), 8);
  } else {
    const size_t need_from_prefix = 8 - suffix.size();
    if (prefix.size() < need_from_prefix) {
      return false;
    }
    memcpy(tmp, prefix.data() + (prefix.size() - need_from_prefix),
           need_from_prefix);
    memcpy(tmp + need_from_prefix, suffix.data(), suffix.size());
  }
  *footer_out = DecodeFixed64(tmp);
  return true;
}

// Compare only the user-key portion (internal key excluding the trailing
// 8-byte seqno/type footer) against `target_user_key`, assuming bytewise
// comparator semantics.
//
// Returns -1 / 0 / +1 in the same sense as Comparator::Compare.
inline int KVSepBptreeCompareUserKeyBytewise(const Slice& prefix,
                                             const Slice& suffix,
                                             const Slice& target_user_key) {
  const size_t total = prefix.size() + suffix.size();
  // Treat malformed keys as "smaller" to keep callers safe; they will likely
  // surface corruption elsewhere.
  if (total < 8) {
    return -1;
  }
  const size_t user_len = total - 8;
  const size_t other_len = target_user_key.size();
  const size_t min_len = std::min(user_len, other_len);

  const size_t prefix_part = std::min(prefix.size(), min_len);
  if (prefix_part > 0) {
    const int c = memcmp(prefix.data(), target_user_key.data(), prefix_part);
    if (c != 0) {
      return c < 0 ? -1 : 1;
    }
  }
  const size_t remain = min_len - prefix_part;
  if (remain > 0) {
    const int c =
        memcmp(suffix.data(), target_user_key.data() + prefix_part, remain);
    if (c != 0) {
      return c < 0 ? -1 : 1;
    }
  }
  if (user_len < other_len) {
    return -1;
  }
  if (user_len > other_len) {
    return 1;
  }
  return 0;
}

// Compare an internal key stored as `prefix || suffix` with a contiguous
// internal key `target_internal_key`, assuming the user comparator is bytewise.
//
// This matches InternalKeyComparator ordering:
//   - increasing user key (bytewise)
//   - decreasing seqno/type footer (numeric, little endian in encoding)
inline int KVSepBptreeCompareInternalKeyBytewise(
    const Slice& prefix, const Slice& suffix, const Slice& target_internal_key) {
  if (target_internal_key.size() < 8) {
    return 1;
  }
  const Slice target_user_key(target_internal_key.data(),
                              target_internal_key.size() - 8);
  int r = KVSepBptreeCompareUserKeyBytewise(prefix, suffix, target_user_key);
  if (r != 0) {
    return r;
  }

  uint64_t entry_footer = 0;
  if (!KVSepBptreeDecodeInternalKeyFooter(prefix, suffix, &entry_footer)) {
    return -1;
  }
  const uint64_t target_footer =
      DecodeFixed64(target_internal_key.data() + target_internal_key.size() - 8);
  if (entry_footer > target_footer) {
    return -1;
  }
  if (entry_footer < target_footer) {
    return 1;
  }
  return 0;
}

// Table property key indicating this SST uses KV-separation encoding.
inline constexpr const char kKVSepBptreeTablePropertyKey[] =
    "rocksdb.experimental.kvsep_bptree";

// User-collected property indicating how many index levels are used by the
// KV-sep B+tree index (1 means "normal" single index block over data blocks).
inline constexpr const char kKVSepBptreeIndexLevelsPropertyKey[] =
    "rocksdb.experimental.kvsep_bptree.index_levels";

// User-collected property indicating which KV-sep leaf block encoding is used.
// 1: leaf entries store full internal keys and a self-contained value pointer
//    (value block handle + off/len) per entry.
// 2: leaf header stores a shared key prefix + per-leaf value block handle, and
//    entries store only key suffix + off/len.
inline constexpr const char kKVSepBptreeLeafFormatVersionPropertyKey[] =
    "rocksdb.experimental.kvsep_bptree.leaf_format_version";

// Per-entry pointer encoding for KV-separation:
//   fixed64 value_block_offset
//   fixed64 value_block_size
//   varint32 value_off_in_block
//   varint32 value_len
//
// This makes each leaf entry self-contained: it can locate the value-only
// block directly without an additional meta mapping.
inline void EncodeKVSepBptreeLeafPointer(const BlockHandle& value_block_handle,
                                        uint32_t value_off, uint32_t value_len,
                                        std::string* out) {
  assert(out != nullptr);
  out->clear();
  out->reserve(8 + 8 + 10 + 10);
  PutFixed64(out, value_block_handle.offset());
  PutFixed64(out, value_block_handle.size());
  PutVarint32(out, value_off);
  PutVarint32(out, value_len);
}

inline Status DecodeKVSepBptreeLeafPointer(const Slice& ptr,
                                          BlockHandle* value_block_handle,
                                          uint32_t* value_off,
                                          uint32_t* value_len) {
  if (value_block_handle == nullptr || value_off == nullptr ||
      value_len == nullptr) {
    return Status::InvalidArgument("kvsep leaf pointer decode: null output");
  }
  const char* p = ptr.data();
  const char* limit = ptr.data() + ptr.size();
  if (limit - p < 16) {
    return Status::Corruption("kvsep leaf pointer too small");
  }
  uint64_t off = DecodeFixed64(p);
  p += 8;
  uint64_t sz = DecodeFixed64(p);
  p += 8;
  value_block_handle->set_offset(off);
  value_block_handle->set_size(sz);
  p = GetVarint32Ptr(p, limit, value_off);
  if (p == nullptr) {
    return Status::Corruption("kvsep leaf pointer decode: bad value_off");
  }
  p = GetVarint32Ptr(p, limit, value_len);
  if (p == nullptr) {
    return Status::Corruption("kvsep leaf pointer decode: bad value_len");
  }
  if (p != limit) {
    return Status::Corruption("kvsep leaf pointer decode: trailing bytes");
  }
  return Status::OK();
}

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

// Leaf V2 view (prefix-compressed leaf encoding).
// Layout:
//   fixed32 version (=2)
//   varint32 prefix_len + prefix bytes
//   fixed64 value_block_offset + fixed64 value_block_size
//   fixed32 num_entries
//   (n+1) * fixed32 suffix_offsets
//   n * fixed32 value_off
//   n * fixed32 value_len
//   concatenated suffix bytes
class KVSepBptreeLeafV2View {
 public:
  Status InitFromContents(const Slice& contents) {
    prefix_ = Slice();
    value_block_handle_ = BlockHandle::NullBlockHandle();
    num_entries_ = 0;
    offsets_ = nullptr;
    value_offs_ = nullptr;
    value_lens_ = nullptr;
    suffix_base_ = nullptr;
    suffix_size_ = 0;

    const char* p = contents.data();
    const char* limit = contents.data() + contents.size();
    if (limit - p < 4) {
      return Status::Corruption("kvsep leaf v2: truncated header");
    }
    const uint32_t version = DecodeFixed32(p);
    p += 4;
    if (version != 2u) {
      return Status::Corruption("kvsep leaf v2: bad version");
    }

    uint32_t prefix_len = 0;
    p = GetVarint32Ptr(p, limit, &prefix_len);
    if (p == nullptr) {
      return Status::Corruption("kvsep leaf v2: bad prefix_len");
    }
    if (static_cast<size_t>(limit - p) < prefix_len + 8 + 8 + 4) {
      return Status::Corruption("kvsep leaf v2: truncated header fields");
    }
    prefix_ = Slice(p, prefix_len);
    p += prefix_len;
    value_block_handle_.set_offset(DecodeFixed64(p));
    p += 8;
    value_block_handle_.set_size(DecodeFixed64(p));
    p += 8;
    num_entries_ = DecodeFixed32(p);
    p += 4;

    const size_t offsets_bytes =
        (static_cast<size_t>(num_entries_) + 1) * 4;
    const size_t value_off_bytes = static_cast<size_t>(num_entries_) * 4;
    const size_t value_len_bytes = static_cast<size_t>(num_entries_) * 4;
    if (static_cast<size_t>(limit - p) <
        offsets_bytes + value_off_bytes + value_len_bytes) {
      return Status::Corruption("kvsep leaf v2: truncated arrays");
    }
    offsets_ = p;
    p += offsets_bytes;
    value_offs_ = p;
    p += value_off_bytes;
    value_lens_ = p;
    p += value_len_bytes;
    suffix_base_ = p;
    suffix_size_ = static_cast<size_t>(limit - p);

    // Validate suffix offsets. (Fast path in release builds.)
    uint32_t last_off = DecodeFixed32(offsets_ + num_entries_ * 4);
    if (last_off != suffix_size_) {
      return Status::Corruption("kvsep leaf v2: bad suffix offsets tail");
    }
#ifndef NDEBUG
    uint32_t prev = 0;
    for (uint32_t i = 0; i <= num_entries_; ++i) {
      uint32_t off = DecodeFixed32(offsets_ + i * 4);
      if (off < prev || off > suffix_size_) {
        return Status::Corruption("kvsep leaf v2: bad suffix offsets");
      }
      prev = off;
    }
#endif
    return Status::OK();
  }

  uint32_t num_entries() const { return num_entries_; }
  const Slice& prefix() const { return prefix_; }
  const BlockHandle& value_block_handle() const { return value_block_handle_; }

  Slice SuffixAt(uint32_t idx) const {
    assert(idx < num_entries_);
    const uint32_t begin = DecodeFixed32(offsets_ + idx * 4);
    const uint32_t end = DecodeFixed32(offsets_ + (idx + 1) * 4);
    assert(end >= begin);
    assert(end <= suffix_size_);
    return Slice(suffix_base_ + begin, static_cast<size_t>(end - begin));
  }

  uint32_t ValueOffAt(uint32_t idx) const {
    assert(idx < num_entries_);
    return DecodeFixed32(value_offs_ + idx * 4);
  }

  uint32_t ValueLenAt(uint32_t idx) const {
    assert(idx < num_entries_);
    return DecodeFixed32(value_lens_ + idx * 4);
  }

  Slice FullKeyAt(uint32_t idx, std::string* scratch) const {
    assert(scratch != nullptr);
    scratch->clear();
    scratch->append(prefix_.data(), prefix_.size());
    const Slice suffix = SuffixAt(idx);
    scratch->append(suffix.data(), suffix.size());
    return Slice(*scratch);
  }

  // A lower-overhead variant for callers that keep the leaf prefix already
  // materialized in the scratch buffer. This avoids clearing and re-appending
  // the prefix on every comparison.
  Slice FullKeyAtWithScratchPrefix(uint32_t idx, std::string* scratch,
                                   size_t scratch_prefix_len) const {
    assert(scratch != nullptr);
    assert(scratch_prefix_len == prefix_.size());
#ifndef NDEBUG
    if (scratch_prefix_len > 0) {
      assert(scratch->size() >= scratch_prefix_len);
      assert(memcmp(scratch->data(), prefix_.data(), scratch_prefix_len) == 0);
    }
#endif
    scratch->resize(scratch_prefix_len);
    const Slice suffix = SuffixAt(idx);
    scratch->append(suffix.data(), suffix.size());
    return Slice(*scratch);
  }

 private:
  Slice prefix_;
  BlockHandle value_block_handle_;
  uint32_t num_entries_ = 0;
  const char* offsets_ = nullptr;
  const char* value_offs_ = nullptr;
  const char* value_lens_ = nullptr;
  const char* suffix_base_ = nullptr;
  size_t suffix_size_ = 0;
};

// Pair block V3: a single on-disk block containing:
//   - leaf key metadata (prefix-compressed)
//   - value bytes
//
// Pair layout:
//   fixed32 version (=3)
//   fixed32 leaf_bytes
//   fixed32 value_bytes
//   leaf_payload bytes (leaf_bytes)
//   value_payload bytes (value_bytes)
//
// Leaf payload layout (leaf version=3):
//   fixed32 version (=3)
//   varint32 prefix_len + prefix bytes
//   fixed32 num_entries
//   (n+1) * fixed32 suffix_offsets
//   n * fixed32 value_off
//   n * fixed32 value_len
//   concatenated suffix bytes
class KVSepBptreePairV3View {
 public:
  Status InitFromContents(const Slice& contents) {
    leaf_contents_ = Slice();
    value_contents_ = Slice();

    const char* p = contents.data();
    const char* limit = contents.data() + contents.size();
    if (limit - p < 12) {
      return Status::Corruption("kvsep pair v3: truncated header");
    }
    const uint32_t version = DecodeFixed32(p);
    p += 4;
    if (version != 3u) {
      return Status::Corruption("kvsep pair v3: bad version");
    }
    const uint32_t leaf_bytes = DecodeFixed32(p);
    p += 4;
    const uint32_t value_bytes = DecodeFixed32(p);
    p += 4;
    if (static_cast<size_t>(limit - p) <
        static_cast<size_t>(leaf_bytes) + static_cast<size_t>(value_bytes)) {
      return Status::Corruption("kvsep pair v3: truncated payload");
    }
    leaf_contents_ = Slice(p, leaf_bytes);
    p += leaf_bytes;
    value_contents_ = Slice(p, value_bytes);
    return Status::OK();
  }

  const Slice& leaf_contents() const { return leaf_contents_; }
  const Slice& value_contents() const { return value_contents_; }

 private:
  Slice leaf_contents_;
  Slice value_contents_;
};

class KVSepBptreeLeafV3View {
 public:
  Status InitFromContents(const Slice& contents) {
    prefix_ = Slice();
    num_entries_ = 0;
    offsets_ = nullptr;
    value_offs_ = nullptr;
    value_lens_ = nullptr;
    suffix_base_ = nullptr;
    suffix_size_ = 0;

    const char* p = contents.data();
    const char* limit = contents.data() + contents.size();
    if (limit - p < 4) {
      return Status::Corruption("kvsep leaf v3: truncated header");
    }
    const uint32_t version = DecodeFixed32(p);
    p += 4;
    if (version != 3u) {
      return Status::Corruption("kvsep leaf v3: bad version");
    }

    uint32_t prefix_len = 0;
    p = GetVarint32Ptr(p, limit, &prefix_len);
    if (p == nullptr) {
      return Status::Corruption("kvsep leaf v3: bad prefix_len");
    }
    if (static_cast<size_t>(limit - p) < prefix_len + 4) {
      return Status::Corruption("kvsep leaf v3: truncated header fields");
    }
    prefix_ = Slice(p, prefix_len);
    p += prefix_len;
    num_entries_ = DecodeFixed32(p);
    p += 4;

    const size_t offsets_bytes =
        (static_cast<size_t>(num_entries_) + 1) * 4;
    const size_t value_off_bytes = static_cast<size_t>(num_entries_) * 4;
    const size_t value_len_bytes = static_cast<size_t>(num_entries_) * 4;
    if (static_cast<size_t>(limit - p) <
        offsets_bytes + value_off_bytes + value_len_bytes) {
      return Status::Corruption("kvsep leaf v3: truncated arrays");
    }
    offsets_ = p;
    p += offsets_bytes;
    value_offs_ = p;
    p += value_off_bytes;
    value_lens_ = p;
    p += value_len_bytes;
    suffix_base_ = p;
    suffix_size_ = static_cast<size_t>(limit - p);

    uint32_t last_off = DecodeFixed32(offsets_ + num_entries_ * 4);
    if (last_off != suffix_size_) {
      return Status::Corruption("kvsep leaf v3: bad suffix offsets tail");
    }
#ifndef NDEBUG
    uint32_t prev = 0;
    for (uint32_t i = 0; i <= num_entries_; ++i) {
      uint32_t off = DecodeFixed32(offsets_ + i * 4);
      if (off < prev || off > suffix_size_) {
        return Status::Corruption("kvsep leaf v3: bad suffix offsets");
      }
      prev = off;
    }
#endif
    return Status::OK();
  }

  uint32_t num_entries() const { return num_entries_; }
  const Slice& prefix() const { return prefix_; }

  Slice SuffixAt(uint32_t idx) const {
    assert(idx < num_entries_);
    const uint32_t begin = DecodeFixed32(offsets_ + idx * 4);
    const uint32_t end = DecodeFixed32(offsets_ + (idx + 1) * 4);
    assert(end >= begin);
    assert(end <= suffix_size_);
    return Slice(suffix_base_ + begin, static_cast<size_t>(end - begin));
  }

  uint32_t ValueOffAt(uint32_t idx) const {
    assert(idx < num_entries_);
    return DecodeFixed32(value_offs_ + idx * 4);
  }

  uint32_t ValueLenAt(uint32_t idx) const {
    assert(idx < num_entries_);
    return DecodeFixed32(value_lens_ + idx * 4);
  }

  Slice FullKeyAt(uint32_t idx, std::string* scratch) const {
    assert(scratch != nullptr);
    scratch->clear();
    scratch->append(prefix_.data(), prefix_.size());
    const Slice suffix = SuffixAt(idx);
    scratch->append(suffix.data(), suffix.size());
    return Slice(*scratch);
  }

  Slice FullKeyAtWithScratchPrefix(uint32_t idx, std::string* scratch,
                                   size_t scratch_prefix_len) const {
    assert(scratch != nullptr);
    assert(scratch_prefix_len == prefix_.size());
#ifndef NDEBUG
    if (scratch_prefix_len > 0) {
      assert(scratch->size() >= scratch_prefix_len);
      assert(memcmp(scratch->data(), prefix_.data(), scratch_prefix_len) == 0);
    }
#endif
    scratch->resize(scratch_prefix_len);
    const Slice suffix = SuffixAt(idx);
    scratch->append(suffix.data(), suffix.size());
    return Slice(*scratch);
  }

 private:
  Slice prefix_;
  uint32_t num_entries_ = 0;
  const char* offsets_ = nullptr;
  const char* value_offs_ = nullptr;
  const char* value_lens_ = nullptr;
  const char* suffix_base_ = nullptr;
  size_t suffix_size_ = 0;
};

}  // namespace ROCKSDB_NAMESPACE
