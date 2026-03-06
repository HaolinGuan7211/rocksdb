#include "db/zigzag_sst_internal_reader.h"

#include <memory>
#include <string>

#include "file/random_access_file_reader.h"
#include "options/cf_options.h"
#include "rocksdb/env.h"
#include "rocksdb/file_system.h"
#include "table/internal_iterator.h"
#include "table/table_builder.h"
#include "table/table_reader.h"

namespace ROCKSDB_NAMESPACE {

struct ZigZagSstInternalReader::Rep {
  explicit Rep(const Options& opts)
      : options(opts),
        env_options(options),
        ioptions(options),
        moptions(ColumnFamilyOptions(options)) {}

  Options options;
  EnvOptions env_options;
  ImmutableOptions ioptions;
  MutableCFOptions moptions;
  std::unique_ptr<TableReader> table_reader;
};

ZigZagSstInternalReader::ZigZagSstInternalReader(const Options& options)
    : rep_(new Rep(options)) {}

ZigZagSstInternalReader::~ZigZagSstInternalReader() = default;

Status ZigZagSstInternalReader::Open(const std::string& file_path) {
  auto* rep = rep_.get();
  const auto& fs = rep->options.env->GetFileSystem();
  FileOptions file_options(rep->env_options);
  uint64_t file_size = 0;
  std::unique_ptr<FSRandomAccessFile> file;
  std::unique_ptr<RandomAccessFileReader> file_reader;

  Status s = fs->GetFileSize(file_path, file_options.io_options, &file_size,
                             nullptr);
  if (s.ok()) {
    s = fs->NewRandomAccessFile(file_path, file_options, &file, nullptr);
  }
  if (s.ok()) {
    file_reader.reset(new RandomAccessFileReader(std::move(file), file_path));
  }
  if (s.ok()) {
    TableReaderOptions table_reader_options(
        rep->ioptions, rep->moptions.prefix_extractor,
        rep->moptions.compression_manager.get(), rep->env_options,
        rep->ioptions.internal_comparator,
        rep->moptions.block_protection_bytes_per_key,
        false /* skip_filters */, false /* immortal */,
        false /* force_direct_prefetch */, -1 /* level */,
        nullptr /* block_cache_tracer */,
        0 /* max_file_size_for_l0_meta_pin */, "" /* cur_db_session_id */,
        0 /* cur_file_num */, {} /* unique_id */, kMaxSequenceNumber,
        0 /* tail_size */,
        rep->ioptions.persist_user_defined_timestamps);
    s = rep->options.table_factory->NewTableReader(
        table_reader_options, std::move(file_reader), file_size,
        &rep->table_reader);
  }
  return s;
}

InternalIterator* ZigZagSstInternalReader::NewIterator(
    const ReadOptions& read_options, TableReaderCaller caller, Arena* arena,
    bool skip_filters) {
  assert(rep_ != nullptr);
  assert(rep_->table_reader != nullptr);
  return rep_->table_reader->NewIterator(
      read_options, rep_->moptions.prefix_extractor.get(), arena, skip_filters,
      caller);
}

FragmentedRangeTombstoneIterator*
ZigZagSstInternalReader::NewRangeTombstoneIterator(
    const ReadOptions& read_options) {
  assert(rep_ != nullptr);
  assert(rep_->table_reader != nullptr);
  return rep_->table_reader->NewRangeTombstoneIterator(read_options);
}

Status ZigZagSstInternalReader::Get(const ReadOptions& read_options,
                                    const Slice& internal_key,
                                    GetContext* get_context,
                                    bool skip_filters) {
  assert(rep_ != nullptr);
  assert(rep_->table_reader != nullptr);
  return rep_->table_reader->Get(read_options, internal_key, get_context,
                                 rep_->moptions.prefix_extractor.get(),
                                 skip_filters);
}

}  // namespace ROCKSDB_NAMESPACE
