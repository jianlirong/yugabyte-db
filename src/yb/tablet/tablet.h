// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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
#ifndef YB_TABLET_TABLET_H_
#define YB_TABLET_TABLET_H_

#include <iosfwd>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "yb/rocksdb/cache.h"
#include "yb/rocksdb/options.h"
#include "yb/rocksdb/statistics.h"
#include "yb/rocksdb/write_batch.h"

#include "yb/tserver/tserver.pb.h"

#include "yb/client/client_fwd.h"

#include "yb/common/iterator.h"
#include "yb/common/predicate_encoder.h"
#include "yb/common/schema.h"
#include "yb/common/row_operations.h"
#include "yb/common/transaction.h"
#include "yb/common/ql_storage_interface.h"

#include "yb/docdb/docdb.pb.h"
#include "yb/docdb/docdb_compaction_filter.h"
#include "yb/docdb/doc_operation.h"
#include "yb/docdb/ql_rocksdb_storage.h"
#include "yb/docdb/shared_lock_manager.h"

#include "yb/gutil/atomicops.h"
#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/macros.h"

#include "yb/tablet/abstract_tablet.h"
#include "yb/tablet/lock_manager.h"
#include "yb/tablet/tablet_options.h"
#include "yb/tablet/mvcc.h"
#include "yb/tablet/rowset.h"
#include "yb/tablet/rowset_metadata.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/tablet/transaction_participant.h"

#include "yb/util/locks.h"
#include "yb/util/metrics.h"
#include "yb/util/pending_op_counter.h"
#include "yb/util/semaphore.h"
#include "yb/util/slice.h"
#include "yb/util/status.h"
#include "yb/util/countdown_latch.h"

namespace rocksdb {
class DB;
}

namespace yb {

class MemTracker;
class MetricEntity;
class RowChangeList;
class UnionIterator;

namespace log {
class LogAnchorRegistry;
}

namespace server {
class Clock;
}

class MaintenanceManager;
class MaintenanceOp;
class MaintenanceOpStats;

namespace tablet {

class AlterSchemaOperationState;
class CompactionPolicy;
class MemRowSet;
class MvccSnapshot;
struct RowOp;
class RowSetsInCompaction;
class RowSetTree;
class ScopedReadOperation;
struct TabletComponents;
struct TabletMetrics;
struct TransactionApplyData;
class TransactionCoordinator;
class TransactionCoordinatorContext;
class TransactionParticipant;
class WriteOperationState;

using docdb::LockBatch;

class TabletFlushStats : public rocksdb::EventListener {
 public:

  void OnFlushScheduled(rocksdb::DB* db) override {
    oldest_write_in_memstore_.store(std::numeric_limits<uint64_t>::max(),
                                    std::memory_order_release);
    num_flushes_++;
  }

  void AboutToWriteToDb(HybridTime hybrid_time) {
    // Atomically do oldest_write_in_memstore_ = min(oldest_write_in_memstore_, hybrid_time)
    uint64_t curr_val = hybrid_time.ToUint64();
    uint64_t prev_val = oldest_write_in_memstore_.load(std::memory_order_acquire);
    while (curr_val < prev_val &&
           !oldest_write_in_memstore_.compare_exchange_weak(prev_val, curr_val)) {}
  }

  // Return the hybrid time of the oldest write in the memstore, or HybridTime::kMax if empty
  HybridTime oldest_write_in_memstore() const {
    return HybridTime(oldest_write_in_memstore_.load(std::memory_order_acquire));
  }

  // Number of flushes scheduled. Updated atomically before scheduling.
  size_t num_flushes() {
    return num_flushes_.load(std::memory_order_acquire);
  }

 private:
  std::atomic<size_t> num_flushes_{0};
  std::atomic<uint64_t> oldest_write_in_memstore_{std::numeric_limits<uint64_t>::max()};
};

enum class FlushMode {
  kSync,
  kAsync,
};

class Tablet : public AbstractTablet, public TransactionIntentApplier {
 public:
  typedef std::map<int64_t, int64_t> MaxIdxToSegmentMap;
  friend class CompactRowSetsOp;
  friend class FlushMRSOp;

  class CompactionFaultHooks;
  class FlushCompactCommonHooks;
  class FlushFaultHooks;
  class Iterator;

  // Create a new tablet.
  //
  // If 'metric_registry' is non-NULL, then this tablet will create a 'tablet' entity
  // within the provided registry. Otherwise, no metrics are collected.
  Tablet(
      const scoped_refptr<TabletMetadata>& metadata,
      const scoped_refptr<server::Clock>& clock,
      const std::shared_ptr<MemTracker>& parent_mem_tracker,
      MetricRegistry* metric_registry,
      const scoped_refptr<log::LogAnchorRegistry>& log_anchor_registry,
      const TabletOptions& tablet_options,
      TransactionParticipantContext* transaction_participant_context,
      TransactionCoordinatorContext* transaction_coordinator_context);

  ~Tablet();

  // Open the tablet.
  // Upon completion, the tablet enters the kBootstrapping state.
  CHECKED_STATUS Open();

  // Mark that the tablet has finished bootstrapping.
  // This transitions from kBootstrapping to kOpen state.
  void MarkFinishedBootstrapping();

  // This can be called to proactively prevent new operations from being handled, even before
  // Shutdown() is called.
  void SetShutdownRequestedFlag();
  bool IsShutdownRequested() const {
    return shutdown_requested_.load(std::memory_order::memory_order_acquire);
  }

  void Shutdown();

  CHECKED_STATUS ImportData(const std::string& source_dir);

  CHECKED_STATUS ApplyIntents(const TransactionApplyData& data) override;

  // Decode the Write (insert/mutate) operations from within a user's request.
  // Either fills in operation_state->row_ops or tx_state->kv_write_batch depending on TableType.
  CHECKED_STATUS DecodeWriteOperations(
      const Schema* client_schema,
      WriteOperationState* operation_state);

  // Kudu-specific. To be removed with the rest of Kudu storage engine. This is a no-op for YB
  // tables.
  //
  // Acquire locks for each of the operations in the given txn.
  //
  // Note that, if this fails, it's still possible that the transaction
  // state holds _some_ of the locks. In that case, we expect that
  // the transaction will still clean them up when it is aborted (or
  // otherwise destructed).
  CHECKED_STATUS AcquireKuduRowLocks(WriteOperationState *operation_state);

  // Finish the Prepare phase of a write transaction.
  //
  // Starts an MVCC transaction and assigns a timestamp for the transaction.
  //
  // This should always be done _after_ any relevant row locks are acquired
  // (using CreatePreparedInsert/CreatePreparedMutate). This ensures that,
  // within each row, timestamps only move forward. If we took a timestamp before
  // getting the row lock, we could have the following situation:
  //
  //   Thread 1         |  Thread 2
  //   ----------------------
  //   Start tx 1       |
  //                    |  Start tx 2
  //                    |  Obtain row lock
  //                    |  Update row
  //                    |  Commit tx 2
  //   Obtain row lock  |
  //   Delete row       |
  //   Commit tx 1
  //
  // This would cause the mutation list to look like: @t1: DELETE, @t2: UPDATE
  // which is invalid, since we expect to be able to be able to replay mutations
  // in increasing timestamp order on a given row.
  //
  // TODO: rename this to something like "FinishPrepare" or "StartApply", since
  // it's not the first thing in a transaction!
  void StartOperation(WriteOperationState* operation_state);

  // Insert a new row into the tablet.
  //
  // The provided 'data' slice should have length equivalent to this
  // tablet's Schema.byte_size().
  //
  // After insert, the row and any referred-to memory (eg for strings)
  // have been copied into internal memory, and thus the provided memory
  // buffer may safely be re-used or freed.
  //
  // Returns Status::AlreadyPresent() if an entry with the same key is already
  // present in the tablet.
  // Returns Status::OK unless allocation fails.
  //
  // Acquires the row lock for the given operation, setting it in the
  // RowOp struct. This also sets the row op's RowSetKeyProbe.
  CHECKED_STATUS AcquireLockForOp(WriteOperationState* operation_state,
      RowOp* op);

  // Signal that the given transaction is about to Apply.
  void StartApplying(WriteOperationState* operation_state);

  // Apply all of the row operations associated with this transaction.
  void ApplyRowOperations(WriteOperationState* operation_state);

  // Apply a single row operation, which must already be prepared.
  // The result is set back into row_op->result
  void ApplyKuduRowOperation(WriteOperationState* operation_state,
      RowOp* row_op);

  // Apply a set of RocksDB row operations.
  void ApplyKeyValueRowOperations(
      const docdb::KeyValueWriteBatchPB& put_batch,
      const consensus::OpId& op_id,
      HybridTime hybrid_time,
      rocksdb::WriteBatch* rocksdb_write_batch = nullptr);

  // Takes a Redis WriteRequestPB as input with its redis_write_batch.
  // Constructs a WriteRequestPB containing a serialized WriteBatch that will be
  // replicated by Raft. (Makes a copy, it is caller's responsibility to deallocate
  // write_request afterwards if it is no longer needed).
  // The operation acquires the necessary locks required to correctly serialize concurrent write
  // operations to same/conflicting part of the key/sub-key space. The locks acquired are returned
  // via the 'keys_locked' vector, so that they may be unlocked later when the operation has been
  // committed.
  CHECKED_STATUS KeyValueBatchFromRedisWriteBatch(
      tserver::WriteRequestPB* redis_write_request,
      LockBatch *keys_locked, vector<RedisResponsePB>* responses);

  CHECKED_STATUS HandleRedisReadRequest(
      HybridTime timestamp, const RedisReadRequestPB& redis_read_request,
      RedisResponsePB* response) override;

  CHECKED_STATUS HandleQLReadRequest(
      HybridTime timestamp, const QLReadRequestPB& ql_read_request,
      const TransactionMetadataPB& transaction_metadata, QLResponsePB* response,
      gscoped_ptr<faststring>* rows_data) override;

  CHECKED_STATUS CreatePagingStateForRead(
      const QLReadRequestPB& ql_read_request, const size_t row_count,
      QLResponsePB* response) const override;

  // The QL equivalent of KeyValueBatchFromRedisWriteBatch, works similarly.
  CHECKED_STATUS KeyValueBatchFromQLWriteBatch(
      tserver::WriteRequestPB* write_request,
      LockBatch *keys_locked, tserver::WriteResponsePB* write_response,
      WriteOperationState* operation_state);

  // The Kudu equivalent of KeyValueBatchFromRedisWriteBatch, works similarly.
  CHECKED_STATUS KeyValueBatchFromKuduRowOps(
      tserver::WriteRequestPB* kudu_write_request,
      LockBatch *keys_locked);

  // Uses primary_key:column_name for key encoding.
  CHECKED_STATUS CreateWriteBatchFromKuduRowOps(const vector<DecodedRowOperation> &row_ops,
                                        yb::docdb::KeyValueWriteBatchPB* write_batch,
                                        LockBatch* keys_locked);

  // Create a RocksDB checkpoint in the provided directory. Only used when table_type_ ==
  // YQL_TABLE_TYPE.
  CHECKED_STATUS CreateCheckpoint(const std::string& dir,
      google::protobuf::RepeatedPtrField<RocksDBFilePB>* rocksdb_files = nullptr);

  // Create a new row iterator which yields the rows as of the current MVCC
  // state of this tablet.
  // The returned iterator is not initialized.
  CHECKED_STATUS NewRowIterator(
      const Schema &projection,
      const boost::optional<TransactionId>& transaction_id,
      gscoped_ptr<RowwiseIterator> *iter) const;

  // Whether the iterator should return results in order.
  enum OrderMode {
    UNORDERED = 0,
    ORDERED = 1
  };

  // Create a new row iterator for some historical snapshot.
  CHECKED_STATUS NewRowIterator(
      const Schema &projection,
      const MvccSnapshot &snap,
      const OrderMode order,
      const boost::optional<TransactionId>& transaction_id,
      gscoped_ptr<RowwiseIterator> *iter) const;

  // Flush the current MemRowSet for this tablet to disk. This swaps
  // in a new (initially empty) MemRowSet in its place.
  //
  // This doesn't flush any DeltaMemStores for any existing RowSets.
  // To do that, call FlushBiggestDMS() for example.
  //
  // For RocksDB backed tables it makes RocksDB Flush.
  CHECKED_STATUS Flush(FlushMode mode);

  // Prepares the transaction context for the alter schema operation.
  // An error will be returned if the specified schema is invalid (e.g.
  // key mismatch, or missing IDs)
  CHECKED_STATUS CreatePreparedAlterSchema(AlterSchemaOperationState *operation_state,
      const Schema* schema);

  // Apply the Schema of the specified transaction.
  // This operation will trigger a flush on the current MemRowSet.
  CHECKED_STATUS AlterSchema(AlterSchemaOperationState* operation_state);

  // Rewind the schema to an earlier version than is written in the on-disk
  // metadata. This is done during bootstrap to roll the schema back to the
  // point in time where the logs-to-be-replayed begin, so we can then decode
  // the operations in the log with the correct schema.
  //
  // REQUIRES: state_ == kBootstrapping
  CHECKED_STATUS RewindSchemaForBootstrap(const Schema& schema,
      int64_t schema_version);

  // Prints current RowSet layout, taking a snapshot of the current RowSet interval
  // tree. Also prints the log of the compaction algorithm as evaluated
  // on the current layout.
  void PrintRSLayout(std::ostream* o);

  // Flags to change the behavior of compaction.
  enum CompactFlag {
    COMPACT_NO_FLAGS = 0,

    // Force the compaction to include all rowsets, regardless of the
    // configured compaction policy. This is currently only used in
    // tests.
        FORCE_COMPACT_ALL = 1 << 0
  };
  typedef int CompactFlags;

  CHECKED_STATUS Compact(CompactFlags flags);

  // Update the statistics for performing a compaction.
  void UpdateCompactionStats(MaintenanceOpStats* stats);

  // Returns the exact current size of the MRS, in bytes. A value greater than 0 doesn't imply
  // that the MRS has data, only that it has allocated that amount of memory.
  // This method takes a read lock on component_lock_ and is thread-safe.
  size_t MemRowSetSize() const;

  // Returns true if the MRS is empty, else false. Doesn't rely on size and
  // actually verifies that the MRS has no elements.
  // This method takes a read lock on component_lock_ and is thread-safe.
  bool MemRowSetEmpty() const;

  // Returns the size in bytes for the MRS's log retention.
  size_t MemRowSetLogRetentionSize(const MaxIdxToSegmentMap& max_idx_to_segment_size) const;

  // Estimate the total on-disk size of this tablet, in bytes.
  size_t EstimateOnDiskSize() const;

  // Get the total size of all the DMS
  size_t DeltaMemStoresSize() const;

  // Same as MemRowSetEmpty(), but for the DMS.
  bool DeltaMemRowSetEmpty() const;

  // Fills in the in-memory size and retention size in bytes for the DMS with the
  // highest retention.
  void GetInfoForBestDMSToFlush(const MaxIdxToSegmentMap& max_idx_to_segment_size,
      int64_t* mem_size, int64_t* retention_size) const;

  // Flushes the DMS with the highest retention.
  CHECKED_STATUS FlushDMSWithHighestRetention(
      const MaxIdxToSegmentMap& max_idx_to_segment_size) const;

  // Flush only the biggest DMS
  CHECKED_STATUS FlushBiggestDMS();

  // Finds the RowSet which has the most separate delta files and
  // issues a minor delta compaction.
  CHECKED_STATUS CompactWorstDeltas(RowSet::DeltaCompactionType type);

  // Get the highest performance improvement that would come from compacting the delta stores
  // of one of the rowsets. If the returned performance improvement is 0, or if 'rs' is NULL,
  // then 'rs' isn't set. Callers who already own compact_select_lock_
  // can call GetPerfImprovementForBestDeltaCompactUnlocked().
  double GetPerfImprovementForBestDeltaCompact(RowSet::DeltaCompactionType type,
      std::shared_ptr<RowSet>* rs) const;

  // Same as GetPerfImprovementForBestDeltaCompact(), but doesn't take a lock on
  // compact_select_lock_.
  double GetPerfImprovementForBestDeltaCompactUnlocked(RowSet::DeltaCompactionType type,
      std::shared_ptr<RowSet>* rs) const;

  // Return the current number of rowsets in the tablet.
  size_t num_rowsets() const;

  // Verbosely dump this entire tablet to the logs. This is only
  // really useful when debugging unit tests failures where the tablet
  // has a very small number of rows.
  CHECKED_STATUS DebugDump(vector<std::string> *lines = NULL);

  const Schema* schema() const {
    return &metadata_->schema();
  }

  // Returns a reference to the key projection of the tablet schema.
  // The schema keys are immutable.
  const Schema& key_schema() const { return key_schema_; }

  // Return the MVCC manager for this tablet.
  MvccManager* mvcc_manager() { return &mvcc_; }

  docdb::SharedLockManager* shared_lock_manager() { return &shared_lock_manager_; }

  std::atomic<int64_t>* monotonic_counter() { return &monotonic_counter_; }

  // Set the conter to at least 'value'.
  void UpdateMonotonicCounter(int64_t value);

  const TabletMetadata *metadata() const { return metadata_.get(); }
  TabletMetadata *metadata() { return metadata_.get(); }

  void SetCompactionHooksForTests(const std::shared_ptr<CompactionFaultHooks> &hooks);
  void SetFlushHooksForTests(const std::shared_ptr<FlushFaultHooks> &hooks);
  void SetFlushCompactCommonHooksForTests(
      const std::shared_ptr<FlushCompactCommonHooks> &hooks);

  // Returns the current MemRowSet id, for tests.
  // This method takes a read lock on component_lock_ and is thread-safe.
  int32_t CurrentMrsIdForTests() const;

  // Runs a major delta major compaction on columns with specified IDs.
  // NOTE: RowSet must presently be a DiskRowSet. (Perhaps the API should be
  // a shared_ptr API for now?)
  //
  // TODO: Handle MVCC to support MemRowSet and handle deltas in DeltaMemStore
  CHECKED_STATUS DoMajorDeltaCompaction(const std::vector<ColumnId>& column_ids,
      std::shared_ptr<RowSet> input_rowset);

  // Method used by tests to retrieve all rowsets of this table. This
  // will be removed once code for selecting the appropriate RowSet is
  // finished and delta files is finished is part of Tablet class.
  void GetRowSetsForTests(vector<std::shared_ptr<RowSet> >* out);

  // Register the maintenance ops associated with this tablet
  void RegisterMaintenanceOps(MaintenanceManager* maintenance_manager);

  // Unregister the maintenance ops associated with this tablet.
  // This method is not thread safe.
  void UnregisterMaintenanceOps();

  const std::string& tablet_id() const override { return metadata_->tablet_id(); }

  // Return the metrics for this tablet.
  // May be NULL in unit tests, etc.
  TabletMetrics* metrics() { return metrics_.get(); }

  // Return handle to the metric entity of this tablet.
  const scoped_refptr<MetricEntity>& GetMetricEntity() const { return metric_entity_; }

  // Returns a reference to this tablet's memory tracker.
  const std::shared_ptr<MemTracker>& mem_tracker() const { return mem_tracker_; }

  TableType table_type() const override { return table_type_; }

  // Returns true if a RocksDB-backed tablet has any SSTables.
  bool HasSSTables() const;

  // Returns the maximum persistent op id from all SSTables in RocksDB.
  yb::OpId MaxPersistentOpId() const;

  // Returns the location of the last rocksdb checkpoint. Used for tests only.
  std::string GetLastRocksDBCheckpointDirForTest() { return last_rocksdb_checkpoint_dir_; }

  // For non-kudu table type fills key-value batch in transaction state request and updates
  // request in state. Due to acquiring locks it can block the thread.
  CHECKED_STATUS AcquireLocksAndPerformDocOperations(WriteOperationState *state);

  static const char* kDMSMemTrackerId;

  // Returns the timestamp corresponding to the oldest active reader. If none exists returns
  // the latest timestamp that is safe to read.
  // This is used to figure out what can be garbage collected during a compaction.
  HybridTime OldestReadPoint() const;

  // The HybridTime of the oldest write that is still not scheduled to be flushed in RocksDB.
  TabletFlushStats* flush_stats() const { return flush_stats_.get(); }

  const scoped_refptr<server::Clock> &clock() const {
    return clock_;
  }

  const Schema& SchemaRef() const override {
    return metadata_->schema();
  }

  const common::QLStorageIf& QLStorage() const override {
    return *ql_storage_;
  }

  // Used from tests
  const std::shared_ptr<rocksdb::Statistics>& rocksdb_statistics() const {
    return rocksdb_statistics_;
  }

  TransactionCoordinator* transaction_coordinator() {
    return transaction_coordinator_.get();
  }

  TransactionParticipant* transaction_participant() const {
    return transaction_participant_.get();
  }

  void ForceRocksDBCompactInTest();

  std::string DocDBDumpStrInTest();

  // Returns last committed write index.
  // The main purpose of this method is to make correct log cleanup when tablet does not have
  // writes.
  int64_t last_committed_write_index() const {
    return last_committed_write_index_.load(std::memory_order_acquire);
  }

  void LostLeadership();

 protected:
  friend class Iterator;
  friend class TabletPeerTest;
  friend class ScopedReadOperation;
  FRIEND_TEST(TestTablet, TestGetLogRetentionSizeForIndex);

  CHECKED_STATUS FlushUnlocked(FlushMode mode);

  // A version of Insert that does not acquire locks and instead assumes that
  // they were already acquired. Requires that handles for the relevant locks
  // and MVCC transaction are present in the transaction state.
  CHECKED_STATUS InsertUnlocked(
      WriteOperationState *operation_state,
      RowOp* insert);

  CHECKED_STATUS KuduColumnarInsertUnlocked(
      WriteOperationState *operation_state,
      RowOp* insert,
      const TabletComponents* comps,
      ProbeStats* stats);

  // A version of MutateRow that does not acquire locks and instead assumes
  // they were already acquired. Requires that handles for the relevant locks
  // and MVCC transaction are present in the transaction state.
  CHECKED_STATUS MutateRowUnlocked(
      WriteOperationState *operation_state,
      RowOp* mutate);

  // Capture a set of iterators which, together, reflect all of the data in the tablet.
  //
  // These iterators are not true snapshot iterators, but they are safe against
  // concurrent modification. They will include all data that was present at the time
  // of creation, and potentially newer data.
  //
  // The returned iterators are not Init()ed.
  // 'projection' must remain valid and unchanged for the lifetime of the returned iterators.
  CHECKED_STATUS CaptureConsistentIterators(const Schema *projection,
      const MvccSnapshot &snap,
      const ScanSpec *spec,
      const boost::optional<TransactionId>& transaction_id,
      vector<std::shared_ptr<RowwiseIterator> > *iters) const;

  CHECKED_STATUS KuduColumnarCaptureConsistentIterators(
      const Schema *projection,
      const MvccSnapshot &snap,
      const ScanSpec *spec,
      vector<std::shared_ptr<RowwiseIterator> > *iters) const;

  CHECKED_STATUS QLCaptureConsistentIterators(
      const Schema *projection,
      const MvccSnapshot &snap,
      const ScanSpec *spec,
      const boost::optional<TransactionId>& transaction_id,
      vector<std::shared_ptr<RowwiseIterator> > *iters) const;

  CHECKED_STATUS StartDocWriteOperation(
      const std::vector<std::unique_ptr<docdb::DocOperation>> &doc_ops,
      LockBatch *keys_locked,
      docdb::KeyValueWriteBatchPB* write_batch);

  CHECKED_STATUS PickRowSetsToCompact(RowSetsInCompaction *picked,
      CompactFlags flags) const;

  CHECKED_STATUS DoCompactionOrFlush(const RowSetsInCompaction &input,
      int64_t mrs_being_flushed);

  CHECKED_STATUS FlushMetadata(const RowSetVector& to_remove,
      const RowSetMetadataVector& to_add,
      int64_t mrs_being_flushed);

  static void ModifyRowSetTree(const RowSetTree& old_tree,
      const RowSetVector& rowsets_to_remove,
      const RowSetVector& rowsets_to_add,
      RowSetTree* new_tree);

  // Swap out a set of rowsets, atomically replacing them with the new rowset
  // under the lock.
  void AtomicSwapRowSets(const RowSetVector &to_remove,
      const RowSetVector &to_add);

  // Same as the above, but without taking the lock. This should only be used
  // in cases where the lock is already held.
  void AtomicSwapRowSetsUnlocked(const RowSetVector &to_remove,
      const RowSetVector &to_add);

  void GetComponents(scoped_refptr<TabletComponents>* comps) const {
    shared_lock<rw_spinlock> lock(component_lock_);
    *comps = components_;
  }

  // Create a new MemRowSet, replacing the current one.
  // The 'old_ms' pointer will be set to the current MemRowSet set before the replacement.
  // If the MemRowSet is not empty it will be added to the 'compaction' input
  // and the MemRowSet compaction lock will be taken to prevent the inclusion
  // in any concurrent compactions.
  CHECKED_STATUS ReplaceMemRowSetUnlocked(RowSetsInCompaction *compaction,
      std::shared_ptr<MemRowSet> *old_ms);

  // TODO: Document me.
  CHECKED_STATUS FlushInternal(const RowSetsInCompaction& input,
      const std::shared_ptr<MemRowSet>& old_ms);

  BloomFilterSizing bloom_sizing() const;

  // Convert the specified read client schema (without IDs) to a server schema (with IDs)
  // This method is used by NewRowIterator().
  CHECKED_STATUS GetMappedReadProjection(const Schema& projection,
      Schema *mapped_projection) const;

  CHECKED_STATUS CheckRowInTablet(const ConstContiguousRow& probe) const;

  CHECKED_STATUS OpenKeyValueTablet();
  CHECKED_STATUS OpenKuduColumnarTablet();

  CHECKED_STATUS KuduDebugDump(vector<std::string> *lines);
  CHECKED_STATUS DocDBDebugDump(vector<std::string> *lines);

  // Helper method to find the rowset that has the DMS with the highest retention.
  std::shared_ptr<RowSet> FindBestDMSToFlush(
      const MaxIdxToSegmentMap& max_idx_to_segment_size) const;

  // Helper method to find how many bytes this index retains.
  static int64_t GetLogRetentionSizeForIndex(int64_t min_log_index,
      const MaxIdxToSegmentMap& max_idx_to_segment_size);

  // Register/Unregister a read operation, with an associated timestamp, for the purpose of
  // tracking the oldest read point.
  void RegisterReaderTimestamp(HybridTime read_point) override;
  void UnregisterReader(HybridTime read_point) override;
  HybridTime SafeTimestampToRead() const override;

  void PrepareTransactionWriteBatch(
      const docdb::KeyValueWriteBatchPB& put_batch,
      HybridTime hybrid_time,
      rocksdb::WriteBatch* rocksdb_write_batch);

  Result<TransactionOperationContextOpt> CreateTransactionOperationContext(
      const TransactionMetadataPB& transaction_metadata) const;

  TransactionOperationContextOpt CreateTransactionOperationContext(
      const boost::optional<TransactionId>& transaction_id) const;

  // Lock protecting schema_ and key_schema_.
  //
  // Writers take this lock in shared mode before decoding and projecting
  // their requests. They hold the lock until after APPLY.
  //
  // Readers take this lock in shared mode only long enough to copy the
  // current schema into the iterator, after which all projection is taken
  // care of based on that copy.
  //
  // On an AlterSchema, this is taken in exclusive mode during Prepare() and
  // released after the schema change has been applied.
  mutable rw_semaphore schema_lock_;

  const Schema key_schema_;

  scoped_refptr<TabletMetadata> metadata_;
  TableType table_type_;

  // Used for tests only.
  std::string last_rocksdb_checkpoint_dir_;

  // Lock protecting access to the 'components_' member (i.e the rowsets in the tablet)
  //
  // Shared mode:
  // - Writers take this in shared mode at the same time as they obtain an MVCC hybrid_time
  //   and capture a reference to components_. This ensures that we can use the MVCC hybrid_time
  //   to determine which writers are writing to which components during compaction.
  // - Readers take this in shared mode while capturing their iterators. This ensures that
  //   they see a consistent view when racing against flush/compact.
  //
  // Exclusive mode:
  // - Flushes/compactions take this lock in order to lock out concurrent updates when
  //   swapping in a new memrowset.
  //
  // NOTE: callers should avoid taking this lock for a long time, even in shared mode.
  // This is because the lock has some concept of fairness -- if, while a long reader
  // is active, a writer comes along, then all future short readers will be blocked.
  // TODO: now that this is single-threaded again, we should change it to rw_spinlock
  mutable rw_spinlock component_lock_;

  // The current components of the tablet. These should always be read
  // or swapped under the component_lock.
  scoped_refptr<TabletComponents> components_;

  scoped_refptr<log::LogAnchorRegistry> log_anchor_registry_;
  std::shared_ptr<MemTracker> mem_tracker_;
  std::shared_ptr<MemTracker> dms_mem_tracker_;

  MetricEntityPtr metric_entity_;
  gscoped_ptr<TabletMetrics> metrics_;
  FunctionGaugeDetacher metric_detacher_;

  int64_t next_mrs_id_ = 0;

  // A pointer to the server's clock.
  scoped_refptr<server::Clock> clock_;

  MvccManager mvcc_;

  // Maps a timestamp to the number active readers with that timestamp.
  // TODO(ENG-961): Check if this is a point of contention. If so, shard it as suggested in D1219.
  std::map<HybridTime, int64_t> active_readers_cnt_;
  mutable std::mutex active_readers_mutex_;

  // This is used for Kudu tables only. Docdb uses shared_lock_manager_. lock_manager_ may be
  // deprecated in future.
  LockManager lock_manager_;

  gscoped_ptr<CompactionPolicy> compaction_policy_;

  // Lock protecting the selection of rowsets for compaction.
  // Only one thread may run the compaction selection algorithm at a time
  // so that they don't both try to select the same rowset.
  mutable std::mutex compact_select_lock_;

  // We take this lock when flushing the tablet's rowsets in Tablet::Flush.  We
  // don't want to have two flushes in progress at once, in case the one which
  // started earlier completes after the one started later.
  mutable Semaphore rowsets_flush_sem_{1};

  // Lock used to serialize the creation of RocksDB checkpoints.
  mutable std::mutex create_checkpoint_lock_;

  enum State {
    kInitialized,
    kBootstrapping,
    kOpen,
    kShutdown
  };
  State state_ = kInitialized;

  // Fault hooks. In production code, these will always be NULL.
  std::shared_ptr<CompactionFaultHooks> compaction_hooks_;
  std::shared_ptr<FlushFaultHooks> flush_hooks_;
  std::shared_ptr<FlushCompactCommonHooks> common_hooks_;

  std::vector<MaintenanceOp*> maintenance_ops_;

  // Statistics for the RocksDB database.
  std::shared_ptr<rocksdb::Statistics> rocksdb_statistics_;

  // RocksDB database for key-value tables.
  std::unique_ptr<rocksdb::DB> rocksdb_;

  std::unique_ptr<common::QLStorageIf> ql_storage_;

  // This is for docdb fine-grained locking.
  docdb::SharedLockManager shared_lock_manager_;

  // For the block cache and memory manager shared across tablets
  TabletOptions tablet_options_;

  // A lightweight way to reject new operations when the tablet is shutting down. This is used to
  // prevent race conditions between destroying the RocksDB instance and read/write operations.
  std::atomic_bool shutdown_requested_{false};

  // This is a special atomic counter per tablet that increases monotonically.
  // It is like timestamp, but doesn't need locks to read or update.
  // This is raft replicated as well. Each replicate message contains the current number.
  // It is guaranteed to keep increasing for committed entries even across tablet server
  // restarts and leader changes.
  std::atomic<int64_t> monotonic_counter_{0};

  // Number of pending operations. We use this to make sure we don't shut down RocksDB before all
  // pending operations are finished. We don't have a strict definition of an "operation" for the
  // purpose of this counter. We simply wait for this counter to go to zero before shutting down
  // RocksDB.
  //
  // This is marked mutable because read path member functions (which are const) are using this.
  mutable yb::util::PendingOperationCounter pending_op_counter_;

  std::shared_ptr<yb::docdb::HistoryRetentionPolicy> retention_policy_;

  std::unique_ptr<TransactionCoordinator> transaction_coordinator_;

  std::unique_ptr<TransactionParticipant> transaction_participant_;

  std::atomic<int64_t> last_committed_write_index_{0};

  // Remembers he HybridTime of the oldest write that is still not scheduled to
  // be flushed in RocksDB.
  std::shared_ptr<TabletFlushStats> flush_stats_;

 private:
  DISALLOW_COPY_AND_ASSIGN(Tablet);
};

typedef std::shared_ptr<Tablet> TabletPtr;

// A helper class to manage read transactions. Grabs and registers a read point with the tablet
// when created, and deregisters the read point when this object is destructed.
class ScopedReadOperation {
 public:
  explicit ScopedReadOperation(AbstractTablet* tablet);

  ~ScopedReadOperation();

  HybridTime GetReadTimestamp();

 private:
  AbstractTablet* tablet_;
  HybridTime timestamp_;
};

// Hooks used in test code to inject faults or other code into interesting
// parts of the compaction code.
class Tablet::CompactionFaultHooks {
 public:
  virtual CHECKED_STATUS PostSelectIterators() { return Status::OK(); }
  virtual ~CompactionFaultHooks() {}
};

class Tablet::FlushCompactCommonHooks {
 public:
  virtual CHECKED_STATUS PostTakeMvccSnapshot() { return Status::OK(); }
  virtual CHECKED_STATUS PostWriteSnapshot() { return Status::OK(); }
  virtual CHECKED_STATUS PostSwapInDuplicatingRowSet() { return Status::OK(); }
  virtual CHECKED_STATUS PostReupdateMissedDeltas() { return Status::OK(); }
  virtual CHECKED_STATUS PostSwapNewRowSet() { return Status::OK(); }
  virtual ~FlushCompactCommonHooks() {}
};

// Hooks used in test code to inject faults or other code into interesting
// parts of the Flush() code.
class Tablet::FlushFaultHooks {
 public:
  virtual CHECKED_STATUS PostSwapNewMemRowSet() { return Status::OK(); }
  virtual ~FlushFaultHooks() {}
};

class Tablet::Iterator : public RowwiseIterator {
 public:
  virtual ~Iterator();

  CHECKED_STATUS Init(ScanSpec *spec) override;

  bool HasNext() const override;

  CHECKED_STATUS NextBlock(RowBlock *dst) override;

  std::string ToString() const override;

  const Schema &schema() const override {
    return projection_;
  }

  void GetIteratorStats(std::vector<IteratorStats>* stats) const override;

 private:
  friend class Tablet;

  DISALLOW_COPY_AND_ASSIGN(Iterator);

  Iterator(
      const Tablet* tablet, const Schema& projection, MvccSnapshot snap,
      const OrderMode order, const boost::optional<TransactionId>& transaction_id);

  const Tablet *tablet_;
  Schema projection_;
  const MvccSnapshot snap_;
  const OrderMode order_;
  const boost::optional<TransactionId> transaction_id_;
  gscoped_ptr<RowwiseIterator> iter_;

  // TODO: we could probably share an arena with the Scanner object inside the
  // tserver, but piping it in would require changing a lot of call-sites.
  Arena arena_;
  RangePredicateEncoder encoder_;
};

// Structure which represents the components of the tablet's storage.
// This structure is immutable -- a transaction can grab it and be sure
// that it won't change.
struct TabletComponents : public RefCountedThreadSafe<TabletComponents> {
  TabletComponents(std::shared_ptr<MemRowSet> mrs,
      std::shared_ptr<RowSetTree> rs_tree);
  const std::shared_ptr<MemRowSet> memrowset;
  const std::shared_ptr<RowSetTree> rowsets;
};

}  // namespace tablet
}  // namespace yb

#endif  // YB_TABLET_TABLET_H_
