#pragma once

#include <memory>
#include <string>

#include "rocksdb/options.h"
#include "rocksdb/status.h"
#include "rocksdb/table_reader_caller.h"
#include "table/get_context.h"
#include "table/internal_iterator.h"

namespace ROCKSDB_NAMESPACE {

class Arena;
class FragmentedRangeTombstoneIterator;

class ZigZagSstInternalReader {
 public:
  explicit ZigZagSstInternalReader(const Options& options);
  ~ZigZagSstInternalReader();

  Status Open(const std::string& file_path);

  InternalIterator* NewIterator(const ReadOptions& read_options,
                                TableReaderCaller caller,
                                Arena* arena = nullptr,
                                bool skip_filters = false);

  FragmentedRangeTombstoneIterator* NewRangeTombstoneIterator(
      const ReadOptions& read_options);

  Status Get(const ReadOptions& read_options, const Slice& internal_key,
             GetContext* get_context, bool skip_filters = false);

 private:
  struct Rep;
  std::unique_ptr<Rep> rep_;
};

}  // namespace ROCKSDB_NAMESPACE
