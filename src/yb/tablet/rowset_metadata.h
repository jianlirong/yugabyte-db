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
#ifndef YB_TABLET_ROWSET_METADATA_H
#define YB_TABLET_ROWSET_METADATA_H

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "yb/common/schema.h"
#include "yb/fs/block_id.h"
#include "yb/fs/fs_manager.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/map-util.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/util/debug-util.h"
#include "yb/util/env.h"
#include "yb/util/locks.h"

namespace yb {

namespace tools {
class FsTool;
} // namespace tools

namespace tablet {

class RowSetMetadataUpdate;
class TabletMetadata;

// Keeps tracks of the RowSet data blocks.
//
// On each tablet MemRowSet flush, a new RowSetMetadata is created,
// and the DiskRowSetWriter will create and write the "immutable" blocks for
// columns, bloom filter and adHoc-Index.
//
// Once the flush is completed and all the blocks are written,
// the RowSetMetadata will be flushed. Currently, there is only a block
// containing all the tablet metadata, so flushing the RowSetMetadata will
// trigger a full TabletMetadata flush.
//
// Metadata writeback can be lazy: usage should generally be:
//
//   1) create new files on disk (durably)
//   2) change in-memory state to point to new files
//   3) make corresponding change in RowSetMetadata in-memory
//   4) trigger asynchronous flush
//
//   callback: when metadata has been written:
//   1) remove old data files from disk
//   2) remove log anchors corresponding to previously in-memory data
//
class RowSetMetadata {
 public:
  typedef std::map<ColumnId, BlockId> ColumnIdToBlockIdMap;

  // Create a new RowSetMetadata
  static CHECKED_STATUS CreateNew(TabletMetadata* tablet_metadata,
                          int64_t id,
                          gscoped_ptr<RowSetMetadata>* metadata);

  // Load metadata from a protobuf which was previously read from disk.
  static CHECKED_STATUS Load(TabletMetadata* tablet_metadata,
                     const RowSetDataPB& pb,
                     gscoped_ptr<RowSetMetadata>* metadata);

  CHECKED_STATUS Flush();

  const std::string ToString() const;

  int64_t id() const { return id_; }

  const Schema& tablet_schema() const {
    return tablet_metadata_->schema();
  }

  void set_bloom_block(const BlockId& block_id) {
    std::lock_guard<LockType> l(lock_);
    DCHECK(bloom_block_.IsNull());
    bloom_block_ = block_id;
  }

  void set_adhoc_index_block(const BlockId& block_id) {
    std::lock_guard<LockType> l(lock_);
    DCHECK(adhoc_index_block_.IsNull());
    adhoc_index_block_ = block_id;
  }

  void SetColumnDataBlocks(const ColumnIdToBlockIdMap& blocks_by_col_id);

  void CommitRedoDeltaDataBlock(int64_t dms_id, const BlockId& block_id);

  void CommitUndoDeltaDataBlock(const BlockId& block_id);

  BlockId bloom_block() const {
    std::lock_guard<LockType> l(lock_);
    return bloom_block_;
  }

  BlockId adhoc_index_block() const {
    std::lock_guard<LockType> l(lock_);
    return adhoc_index_block_;
  }

  bool has_adhoc_index_block() const {
    std::lock_guard<LockType> l(lock_);
    return !adhoc_index_block_.IsNull();
  }

  BlockId column_data_block_for_col_id(ColumnId col_id) {
    std::lock_guard<LockType> l(lock_);
    return FindOrDie(blocks_by_col_id_, col_id);
  }

  ColumnIdToBlockIdMap GetColumnBlocksById() const {
    std::lock_guard<LockType> l(lock_);
    return blocks_by_col_id_;
  }

  vector<BlockId> redo_delta_blocks() const {
    std::lock_guard<LockType> l(lock_);
    return redo_delta_blocks_;
  }

  vector<BlockId> undo_delta_blocks() const {
    std::lock_guard<LockType> l(lock_);
    return undo_delta_blocks_;
  }

  TabletMetadata *tablet_metadata() const { return tablet_metadata_; }

  int64_t last_durable_redo_dms_id() const {
    std::lock_guard<LockType> l(lock_);
    return last_durable_redo_dms_id_;
  }

  void SetLastDurableRedoDmsIdForTests(int64_t redo_dms_id) {
    std::lock_guard<LockType> l(lock_);
    last_durable_redo_dms_id_ = redo_dms_id;
  }

  bool HasDataForColumnIdForTests(ColumnId col_id) const {
    BlockId b;
    std::lock_guard<LockType> l(lock_);
    if (!FindCopy(blocks_by_col_id_, col_id, &b)) return false;
    return fs_manager()->BlockExists(b);
  }

  bool HasBloomDataBlockForTests() const {
    std::lock_guard<LockType> l(lock_);
    return !bloom_block_.IsNull() && fs_manager()->BlockExists(bloom_block_);
  }

  FsManager *fs_manager() const { return tablet_metadata_->fs_manager(); }

  // Atomically commit a set of changes to this object.
  //
  // On success, calls TabletMetadata::AddOrphanedBlocks() on the removed blocks.
  CHECKED_STATUS CommitUpdate(const RowSetMetadataUpdate& update);

  std::vector<BlockId> GetAllBlocks();

 private:
  friend class TabletMetadata;
  friend class yb::tools::FsTool;

  typedef simple_spinlock LockType;

  explicit RowSetMetadata(TabletMetadata *tablet_metadata)
    : tablet_metadata_(tablet_metadata),
      initted_(false),
      last_durable_redo_dms_id_(kNoDurableMemStore) {
  }

  RowSetMetadata(TabletMetadata *tablet_metadata,
                 int64_t id)
    : tablet_metadata_(DCHECK_NOTNULL(tablet_metadata)),
      initted_(true),
      id_(id),
      last_durable_redo_dms_id_(kNoDurableMemStore) {
  }

  CHECKED_STATUS InitFromPB(const RowSetDataPB& pb);

  void ToProtobuf(RowSetDataPB *pb);

  TabletMetadata* const tablet_metadata_;
  bool initted_;
  int64_t id_;

  // Protects the below mutable fields.
  mutable LockType lock_;

  BlockId bloom_block_;
  BlockId adhoc_index_block_;

  // Map of column ID to block ID.
  ColumnIdToBlockIdMap blocks_by_col_id_;
  std::vector<BlockId> redo_delta_blocks_;
  std::vector<BlockId> undo_delta_blocks_;

  int64_t last_durable_redo_dms_id_;

  DISALLOW_COPY_AND_ASSIGN(RowSetMetadata);
};

// A set up of updates to be made to a RowSetMetadata object.
// Updates can be collected here, and then atomically applied to a RowSetMetadata
// using the CommitUpdate() function.
class RowSetMetadataUpdate {
 public:
  RowSetMetadataUpdate();
  ~RowSetMetadataUpdate();

  // Replace the subsequence of redo delta blocks with the new (compacted) delta blocks.
  // The replaced blocks must be a contiguous subsequence of the the full list,
  // since delta files cannot overlap in time.
  // 'to_add' may be empty, in which case the blocks in to_remove are simply removed
  // with no replacement.
  RowSetMetadataUpdate& ReplaceRedoDeltaBlocks(const std::vector<BlockId>& to_remove,
                                               const std::vector<BlockId>& to_add);

  // Replace the CFile for the given column ID.
  RowSetMetadataUpdate& ReplaceColumnId(ColumnId col_id, const BlockId& block_id);

  // Remove the CFile for the given column ID.
  RowSetMetadataUpdate& RemoveColumnId(ColumnId col_id);

  // Add a new UNDO delta block to the list of UNDO files.
  // We'll need to replace them instead when we start GCing.
  RowSetMetadataUpdate& SetNewUndoBlock(const BlockId& undo_block);

 private:
  friend class RowSetMetadata;
  RowSetMetadata::ColumnIdToBlockIdMap cols_to_replace_;
  std::vector<ColumnId> col_ids_to_remove_;
  std::vector<BlockId> new_redo_blocks_;

  struct ReplaceDeltaBlocks {
    std::vector<BlockId> to_remove;
    std::vector<BlockId> to_add;
  };
  std::vector<ReplaceDeltaBlocks> replace_redo_blocks_;
  BlockId new_undo_block_;

  DISALLOW_COPY_AND_ASSIGN(RowSetMetadataUpdate);
};

} // namespace tablet
} // namespace yb
#endif /* YB_TABLET_ROWSET_METADATA_H */
