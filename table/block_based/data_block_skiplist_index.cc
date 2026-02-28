// Copyright (c) 2011-present, Facebook, Inc. All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "table/block_based/data_block_skiplist_index.h"

#include <cassert>
#include <cstring>

#include "util/coding.h"

namespace ROCKSDB_NAMESPACE {

namespace {

constexpr uint32_t kSkipListMagic = 0x534B4C50u;  // 'SKLP'
constexpr uint16_t kSkipListVersion = 1;

constexpr size_t kHeaderBytes =
    sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint32_t);
constexpr size_t kEntryBytes = sizeof(uint32_t) * 3;

inline const char* DecodeRestartKeyFromEntry(const char* entry_ptr,
                                             const char* limit,
                                             bool use_value_delta_encoding,
                                             uint32_t* non_shared) {
  uint32_t shared = 0;
  const char* p = GetVarint32Ptr(entry_ptr, limit, &shared);
  if (p == nullptr) {
    return nullptr;
  }
  p = GetVarint32Ptr(p, limit, non_shared);
  if (p == nullptr) {
    return nullptr;
  }
  if (shared != 0) {
    return nullptr;
  }
  if (!use_value_delta_encoding) {
    uint32_t value_length = 0;
    p = GetVarint32Ptr(p, limit, &value_length);
    if (p == nullptr) {
      return nullptr;
    }
  }
  return p;  // points to key bytes
}

}  // namespace

bool DataBlockSkipListIndexBuilder::Build(
    const std::string& block_data, const std::vector<uint32_t>& restart_offsets,
    bool use_value_delta_encoding, std::string* out) const {
  assert(out);
  out->clear();
  if (!enabled_) {
    return false;
  }
  if (stride_restarts_ == 0 || restart_offsets.empty()) {
    return false;
  }

  const char* data = block_data.data();
  const size_t data_size = block_data.size();
  const char* limit = data + data_size;

  // Select restart indices.
  std::vector<uint32_t> selected_restarts;
  selected_restarts.reserve(restart_offsets.size() / stride_restarts_ + 2);
  for (uint32_t r = 0; r < restart_offsets.size(); r += stride_restarts_) {
    selected_restarts.push_back(r);
  }
  // Always include the last restart to keep the final range bounded.
  if (selected_restarts.empty() ||
      selected_restarts.back() != restart_offsets.size() - 1) {
    selected_restarts.push_back(
        static_cast<uint32_t>(restart_offsets.size() - 1));
  }
  // Ensure the first restart is included.
  if (selected_restarts.front() != 0) {
    selected_restarts.insert(selected_restarts.begin(), 0);
  }

  struct TmpEntry {
    uint32_t restart_index;
    const char* key_ptr;
    uint32_t key_size;
  };
  std::vector<TmpEntry> entries;
  entries.reserve(selected_restarts.size());

  for (uint32_t restart_index : selected_restarts) {
    if (restart_index >= restart_offsets.size()) {
      return false;
    }
    uint32_t offset = restart_offsets[restart_index];
    if (offset >= data_size) {
      return false;
    }
    uint32_t key_size = 0;
    const char* key_ptr = DecodeRestartKeyFromEntry(
        data + offset, limit, use_value_delta_encoding, &key_size);
    if (key_ptr == nullptr) {
      return false;
    }
    if (key_ptr + key_size > limit) {
      return false;
    }
    entries.push_back(TmpEntry{restart_index, key_ptr, key_size});
  }

  // Build serialized blob into *out.
  out->reserve(kHeaderBytes + entries.size() * kEntryBytes + 128);

  PutFixed32(out, kSkipListMagic);
  PutFixed16(out, kSkipListVersion);
  PutFixed16(out, stride_restarts_);
  PutFixed32(out, static_cast<uint32_t>(entries.size()));

  const size_t entries_offset = out->size();
  out->resize(entries_offset + entries.size() * kEntryBytes);

  for (size_t i = 0; i < entries.size(); ++i) {
    const TmpEntry& e = entries[i];
    uint32_t key_offset = static_cast<uint32_t>(out->size());
    out->append(e.key_ptr, e.key_size);

    char* dst = &(*out)[entries_offset + i * kEntryBytes];
    EncodeFixed32(dst + 0, e.restart_index);
    EncodeFixed32(dst + 4, key_offset);
    EncodeFixed32(dst + 8, e.key_size);
  }

  return true;
}

void DataBlockSkipListIndex::Initialize(const char* data, uint16_t size) {
  data_ = nullptr;
  size_ = 0;
  num_entries_ = 0;
  stride_restarts_ = 0;

  if (data == nullptr || size < kHeaderBytes) {
    return;
  }

  const uint32_t magic = DecodeFixed32(data);
  if (magic != kSkipListMagic) {
    return;
  }
  const uint16_t version = DecodeFixed16(data + sizeof(uint32_t));
  if (version != kSkipListVersion) {
    return;
  }
  stride_restarts_ =
      DecodeFixed16(data + sizeof(uint32_t) + sizeof(uint16_t));
  num_entries_ = DecodeFixed32(data + sizeof(uint32_t) + sizeof(uint16_t) +
                               sizeof(uint16_t));

  if (stride_restarts_ == 0 || num_entries_ == 0) {
    return;
  }

  const size_t entries_bytes = static_cast<size_t>(num_entries_) * kEntryBytes;
  if (kHeaderBytes + entries_bytes > size) {
    return;
  }

  // Validate offsets.
  for (uint32_t i = 0; i < num_entries_; ++i) {
    const char* entry = data + kHeaderBytes + i * kEntryBytes;
    const uint32_t key_offset = DecodeFixed32(entry + 4);
    const uint32_t key_size = DecodeFixed32(entry + 8);
    if (key_offset < kHeaderBytes + entries_bytes) {
      return;
    }
    if (static_cast<size_t>(key_offset) + static_cast<size_t>(key_size) >
        static_cast<size_t>(size)) {
      return;
    }
  }

  data_ = data;
  size_ = size;
}

uint32_t DataBlockSkipListIndex::RestartIndex(uint32_t i) const {
  assert(Valid());
  assert(i < num_entries_);
  const char* entry = data_ + kHeaderBytes + i * kEntryBytes;
  return DecodeFixed32(entry + 0);
}

Slice DataBlockSkipListIndex::Key(uint32_t i) const {
  assert(Valid());
  assert(i < num_entries_);
  const char* entry = data_ + kHeaderBytes + i * kEntryBytes;
  const uint32_t key_offset = DecodeFixed32(entry + 4);
  const uint32_t key_size = DecodeFixed32(entry + 8);
  assert(static_cast<size_t>(key_offset) + static_cast<size_t>(key_size) <=
         static_cast<size_t>(size_));
  return Slice(data_ + key_offset, key_size);
}

}  // namespace ROCKSDB_NAMESPACE

