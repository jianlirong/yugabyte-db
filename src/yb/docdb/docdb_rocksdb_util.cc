// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/docdb/docdb_rocksdb_util.h"

#include <memory>

#include "yb/common/transaction.h"

#include "yb/rocksdb/rate_limiter.h"
#include "yb/rocksdb/table.h"

#include "yb/docdb/intent_aware_iterator.h"
#include "yb/rocksutil/yb_rocksdb.h"
#include "yb/rocksutil/yb_rocksdb_logger.h"
#include "yb/server/hybrid_clock.h"
#include "yb/util/trace.h"
#include "yb/util/logging.h"

DEFINE_int32(rocksdb_max_background_flushes, 1, "Number threads to do background flushes.");
DEFINE_bool(rocksdb_disable_compactions, false, "Disable background compactions.");
DEFINE_int32(rocksdb_base_background_compactions, 2,
             "Number threads to do background compactions.");
DEFINE_int32(rocksdb_max_background_compactions, 4,
             "Increased number of threads to do background compactions (used when compactions need "
             "to catch up.)");
DEFINE_int32(rocksdb_level0_file_num_compaction_trigger, 5,
             "Number of files to trigger level-0 compaction. -1 if compaction should not be "
             "triggered by number of files at all.");

DEFINE_int32(rocksdb_level0_slowdown_writes_trigger, 24,
             "The number of files above which writes are slowed down.");
DEFINE_int32(rocksdb_level0_stop_writes_trigger, 48,
             "The number of files above which compactions are stopped.");
DEFINE_int32(rocksdb_universal_compaction_size_ratio, 20,
             "The percentage upto which files that are larger are include in a compaction.");
DEFINE_int32(rocksdb_universal_compaction_min_merge_width, 4,
             "The minimum number of files in a single compaction run.");
DEFINE_int64(rocksdb_compact_flush_rate_limit_bytes_per_sec, 100 * 1024 * 1024,
             "Use to control write rate of flush and compaction.");
DEFINE_uint64(rocksdb_compaction_size_threshold_bytes, 2ULL * 1024 * 1024 * 1024,
             "Threshold beyond which compaction is considered large.");
DEFINE_uint64(rocksdb_max_file_size_for_compaction, 0,
             "Maximal allowed file size to participate in RocksDB compaction. 0 - unlimited.");

DEFINE_int64(db_block_size_bytes, 32 * 1024,
             "Size of RocksDB block (in bytes).");

DEFINE_bool(use_docdb_aware_bloom_filter, true,
            "Whether to use the DocDbAwareFilterPolicy for both bloom storage and seeks.");
DEFINE_int32(max_nexts_to_avoid_seek, 8,
             "The number of next calls to try before doing resorting to do a rocksdb seek.");
DEFINE_bool(trace_docdb_calls, false, "Whether we should trace calls into the docdb.");

DEFINE_uint64(initial_seqno, 1ULL << 50, "Initial seqno for new RocksDB instances.");

using std::shared_ptr;
using std::string;
using std::unique_ptr;
using strings::Substitute;

namespace yb {
namespace docdb {

std::shared_ptr<rocksdb::BoundaryValuesExtractor> DocBoundaryValuesExtractorInstance();

Status SeekToValidKvAtTs(
    rocksdb::Iterator *iter,
    const rocksdb::Slice &search_key,
    HybridTime hybrid_time,
    SubDocKey *found_key,
    bool *is_found,
    Value *found_value) {

  // Note: is_found and found_value are allowed to be nullptr, but found_key is not, because we
  // always need to store the key somewhere.
  DCHECK_ONLY_NOTNULL(found_key);

  if (is_found != nullptr) {
    *is_found = true;
  }

  KeyBytes seek_key_bytes = KeyBytes(search_key);

  // TODO: use a utility function for appending the ValueType followed by encoded HybridTime.
  AppendDocHybridTime(DocHybridTime(hybrid_time, kMaxWriteId), &seek_key_bytes);

  // If we end up at a descendant of the search key (i.e. a key that belongs to its subdocument),
  // the timestamp may be greater than hybrid_time, we skip over those cases in a loop.  In other
  // words, the while loop is here only because of optional init markers. In the case of required
  // init markers we will always encounter the parent key (the init marker) before encountering a
  // subdocument key.
  while (true) {
    ROCKSDB_SEEK(iter, seek_key_bytes.AsSlice());
    if (!iter->Valid() || !iter->key().starts_with(search_key)) {
      if (is_found != nullptr) {
        *is_found = false;
      }
      return Status::OK();
    }

    DocHybridTime ht_from_found_key;
    RETURN_NOT_OK(ht_from_found_key.DecodeFromEnd(iter->key()));
    if (ht_from_found_key.hybrid_time() <= hybrid_time) {
      break;
    }
    // We found a key/value pair that is too new compared to the HybridTime we're trying to read
    // at, and also belongs to a subdocument of search_key.
    //
    // Example:
    //
    // Suppose search_key = a and hybrid_time = 15, and there is no init marker for "a".
    // Then we'll find a.b and get here, and we'll have to skip it to go to a.c, which is what
    // we're looking for (the first valid key/value pair for this subdocument).
    //
    // a.b @ HT(20)
    // a.c @ HT(10)

    seek_key_bytes = KeyBytes(iter->key());
    // Continuing the example above, we would seek at a.b @ HT(15) and find a.c @ HT(10) on the next
    // loop iteration.
    RETURN_NOT_OK(seek_key_bytes.ReplaceLastHybridTimeForSeek(hybrid_time));
  }

  rocksdb::Slice value = iter->value();
  RETURN_NOT_OK(found_key->FullyDecodeFrom(iter->key()));
  MonoDelta ttl;
  RETURN_NOT_OK(Value::DecodeTTL(&value, &ttl));
  if (!ttl.Equals(Value::kMaxTtl)) {
    const HybridTime expiry =
        server::HybridClock::AddPhysicalTimeToHybridTime(found_key->hybrid_time(), ttl);
    if (hybrid_time.CompareTo(expiry) > 0) {
      if (found_value != nullptr) {
        *found_value = Value(PrimitiveValue(ValueType::kTombstone));
      }
      // Pretend that the tombstone that we are generating instead of the expired value was written
      // at the expiration time of that value. As of 04/13/2017 we are not relying on this to
      // expire entire subdocuments by adding a TTL to the object marker (we're adding TTLs for
      // every column and every collection element in CQL instead), but logically this is probably
      // what we want.
      found_key->SetHybridTimeForReadPath(expiry);
      return Status::OK();
    }
  }
  if (found_value != nullptr) {
    RETURN_NOT_OK(found_value->Decode(value));
  }
  return Status::OK();
}

void SeekForward(const rocksdb::Slice& slice, rocksdb::Iterator *iter) {
  if (!iter->Valid() || iter->key().compare(slice) >= 0) {
    return;
  }
  ROCKSDB_SEEK(iter, slice);
}

void SeekForward(const KeyBytes& key_bytes, rocksdb::Iterator *iter) {
  SeekForward(key_bytes.AsSlice(), iter);
}

void SeekPastSubKey(const SubDocKey& sub_doc_key, rocksdb::Iterator* iter) {
  KeyBytes key_bytes = sub_doc_key.Encode(/* include_hybrid_time */ false);
  AppendDocHybridTime(DocHybridTime::kMin, &key_bytes);
  SeekForward(key_bytes, iter);
}

void PerformRocksDBSeek(
    rocksdb::Iterator *iter,
    const rocksdb::Slice &seek_key,
    const char* file_name,
    int line) {
#ifndef NDEBUG
  {
    // Validating that we're only using keys with a max "write id" component, or no HybridTime at
    // all, or a minimum possible DocHybridTime, for seeks.
    // - Reading at a HybridTime requires setting WriteId to kMaxWriteId so that we don't read
    //   a database state that only existed in a middle of a single-shard transaction.
    // - Seeking to a key with no DocHybridTime is useful in the write-path InternalDocIterator. The
    //   same effect could have been achieved by using the maximum possible DocHybridTime.
    // - Seeking to a key with a minimum possible DocHybridTime is useful so we can skip the
    //   "top-of-the-row" (or "top-of-the-SubDocument") section (say, "a") and jump to the section
    //   containing its subdocuments (say, a.b, a.c, etc.)
    DocHybridTime dht;
    if (DecodeHybridTimeFromEndOfKey(seek_key, &dht).ok()) {
      if (dht.write_id() != kMaxWriteId && dht != DocHybridTime::kMin) {
        // Sometimes there is no timestamp at the end of a seek key, but it might look like there
        // is one. Before we crash, let's decode the full key and check if the timestamp is really
        // there.
        SubDocKey subdoc_key;
        const Status subdoc_key_decode_status = subdoc_key.FullyDecodeFrom(
            seek_key, /* require_hybrid_time = */ false);
        // Don't crash if we failed to decode the SubDocKey (that is sometimes possible in
        // special-case seek keys that we construct), or if we decoded it and it had no hybrid
        // time (which is used in the write-path InternalDocIterator to check if an object init
        // marker is present).
        if (subdoc_key_decode_status.ok() && subdoc_key.has_hybrid_time()) {
          DCHECK(dht.write_id() == kMaxWriteId || dht == DocHybridTime::kMin)
            << "Trying to seek to a key with a write id that is not maximum possible: "
            << BestEffortDocDBKeyToStr(seek_key)
            << ", hybrid time: " << dht.ToString();
        }
      }
    }
  }
#endif

  int next_count = 0;
  int seek_count = 0;
  if (seek_key.size() == 0) {
    iter->SeekToFirst();
  } else if (!iter->Valid() || iter->key().compare(seek_key) > 0) {
    iter->Seek(seek_key);
  } else {
    for (int nexts = 0; nexts <= FLAGS_max_nexts_to_avoid_seek; nexts++) {
      if (!iter->Valid() || iter->key().compare(seek_key) >= 0) {
        if (FLAGS_trace_docdb_calls) {
          TRACE("Did $0 Next(s) instead of a Seek", nexts);
        }
        break;
      }
      if (nexts < FLAGS_max_nexts_to_avoid_seek) {
        iter->Next();
        ++next_count;
      } else {
        if (FLAGS_trace_docdb_calls) {
          TRACE("Forced to do an actual Seek after $0 Next(s)", FLAGS_max_nexts_to_avoid_seek);
        }
        iter->Seek(seek_key);
        ++seek_count;
      }
    }
  }
  VLOG(4) << Substitute(
      "PerformRocksDBSeek at $0:$1:\n"
      "    Seek key:         $2\n"
      "    Seek key (raw):   $3\n"
      "    Actual key:       $4\n"
      "    Actual key (raw): $5\n"
      "    Actual value:     $6\n"
      "    Next() calls:     $7\n"
      "    Seek() calls:     $8\n",
      file_name, line,
      BestEffortDocDBKeyToStr(seek_key),
      FormatRocksDBSliceAsStr(seek_key),
      iter->Valid() ? BestEffortDocDBKeyToStr(KeyBytes(iter->key())) : "N/A",
      iter->Valid() ? FormatRocksDBSliceAsStr(iter->key()) : "N/A",
      iter->Valid() ? FormatRocksDBSliceAsStr(iter->value()) : "N/A",
      next_count,
      seek_count);
}

namespace {

rocksdb::ReadOptions PrepareReadOptions(
    rocksdb::DB* rocksdb,
    BloomFilterMode bloom_filter_mode,
    const boost::optional<const Slice>& user_key_for_filter,
    const rocksdb::QueryId query_id,
    std::shared_ptr<rocksdb::ReadFileFilter> file_filter) {
  rocksdb::ReadOptions read_opts;
  read_opts.query_id = query_id;
  if (FLAGS_use_docdb_aware_bloom_filter &&
    bloom_filter_mode == BloomFilterMode::USE_BLOOM_FILTER) {
    DCHECK(user_key_for_filter);
    read_opts.table_aware_file_filter = rocksdb->GetOptions().table_factory->
        NewTableAwareReadFileFilter(read_opts, user_key_for_filter.get());
  }
  read_opts.file_filter = std::move(file_filter);
  return read_opts;
}

} // namespace

unique_ptr<rocksdb::Iterator> CreateRocksDBIterator(
    rocksdb::DB* rocksdb,
    BloomFilterMode bloom_filter_mode,
    const boost::optional<const Slice>& user_key_for_filter,
    const rocksdb::QueryId query_id,
    std::shared_ptr<rocksdb::ReadFileFilter> file_filter) {
  return unique_ptr<rocksdb::Iterator>(rocksdb->NewIterator(PrepareReadOptions(rocksdb,
      bloom_filter_mode, user_key_for_filter, query_id, std::move(file_filter))));
}

unique_ptr<IntentAwareIterator> CreateIntentAwareIterator(
    rocksdb::DB* rocksdb,
    BloomFilterMode bloom_filter_mode,
    const boost::optional<const Slice>& user_key_for_filter,
    const rocksdb::QueryId query_id,
    const TransactionOperationContextOpt& txn_op_context,
    const HybridTime high_ht,
    std::shared_ptr<rocksdb::ReadFileFilter> file_filter) {
  rocksdb::ReadOptions read_opts = PrepareReadOptions(rocksdb, bloom_filter_mode,
      user_key_for_filter, query_id, std::move(file_filter));
  return std::make_unique<IntentAwareIterator>(rocksdb, read_opts, high_ht, txn_op_context);
}

void InitRocksDBOptions(
    rocksdb::Options* options, const string& tablet_id,
    const shared_ptr<rocksdb::Statistics>& statistics,
    const tablet::TabletOptions& tablet_options) {
  options->create_if_missing = true;
  options->disableDataSync = true;
  options->statistics = statistics;
  options->info_log = std::make_shared<YBRocksDBLogger>(Substitute("T $0: ", tablet_id));
  options->info_log_level = YBRocksDBLogger::ConvertToRocksDBLogLevel(FLAGS_minloglevel);
  options->initial_seqno = FLAGS_initial_seqno;
  options->boundary_extractor = DocBoundaryValuesExtractorInstance();
  options->memory_monitor = tablet_options.memory_monitor;
  options->listeners.insert(
      options->listeners.end(), tablet_options.listeners.begin(),
      tablet_options.listeners.end()); // Append listeners

  // Set block cache options.
  rocksdb::BlockBasedTableOptions table_options;
  if (tablet_options.block_cache) {
    table_options.block_cache = tablet_options.block_cache;
    // Cache the bloom filters in the block cache.
    table_options.cache_index_and_filter_blocks = true;
  } else {
    table_options.no_block_cache = true;
    table_options.cache_index_and_filter_blocks = false;
  }
  table_options.block_size = FLAGS_db_block_size_bytes;

  // Set our custom bloom filter that is docdb aware.
  if (FLAGS_use_docdb_aware_bloom_filter) {
    table_options.filter_policy.reset(new DocDbAwareFilterPolicy(
        table_options.filter_block_size * 8, options->info_log.get()));
  }

  options->table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

  // Compaction related options.

  // Enable universal style compactions.
  bool compactions_enabled = !FLAGS_rocksdb_disable_compactions;
  options->compaction_style = compactions_enabled
    ? rocksdb::CompactionStyle::kCompactionStyleUniversal
    : rocksdb::CompactionStyle::kCompactionStyleNone;
  // Set the number of levels to 1.
  options->num_levels = 1;

  if (compactions_enabled) {
    options->base_background_compactions = FLAGS_rocksdb_base_background_compactions;
    options->max_background_compactions = FLAGS_rocksdb_max_background_compactions;
    options->max_background_flushes = FLAGS_rocksdb_max_background_flushes;
    options->level0_file_num_compaction_trigger = FLAGS_rocksdb_level0_file_num_compaction_trigger;
    options->level0_slowdown_writes_trigger = FLAGS_rocksdb_level0_slowdown_writes_trigger;
    options->level0_stop_writes_trigger = FLAGS_rocksdb_level0_stop_writes_trigger;
    // This determines the algo used to compute which files will be included. The "total size" based
    // computation compares the size of every new file with the sum of all files included so far.
    options->compaction_options_universal.stop_style =
        rocksdb::CompactionStopStyle::kCompactionStopStyleTotalSize;
    options->compaction_options_universal.size_ratio =
        FLAGS_rocksdb_universal_compaction_size_ratio;
    options->compaction_options_universal.min_merge_width =
        FLAGS_rocksdb_universal_compaction_min_merge_width;
    options->compaction_size_threshold_bytes = FLAGS_rocksdb_compaction_size_threshold_bytes;
    if (FLAGS_rocksdb_compact_flush_rate_limit_bytes_per_sec > 0) {
      options->rate_limiter.reset(
          rocksdb::NewGenericRateLimiter(FLAGS_rocksdb_compact_flush_rate_limit_bytes_per_sec));
    }
  }

  uint64_t max_file_size_for_compaction = FLAGS_rocksdb_max_file_size_for_compaction;
  if (max_file_size_for_compaction != 0) {
    options->max_file_size_for_compaction = max_file_size_for_compaction;
  }
}

}  // namespace docdb
}  // namespace yb
