//
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
//

#include "yb/tablet/operations/update_txn_operation.h"

#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/transaction_coordinator.h"

using namespace std::literals;

namespace yb {
namespace tablet {

void UpdateTxnOperationState::UpdateRequestFromConsensusRound() {
  request_ = consensus_round()->replicate_msg()->mutable_transaction_state();
}

std::string UpdateTxnOperationState::ToString() const {
  return Format("UpdateTxnOperationState [$0]",
                request_ ? "(none)"s : request_->ShortDebugString());
}

consensus::ReplicateMsgPtr UpdateTxnOperation::NewReplicateMsg() {
  auto result = std::make_shared<consensus::ReplicateMsg>();
  result->set_op_type(consensus::UPDATE_TRANSACTION_OP);
  *result->mutable_transaction_state() = *state()->request();
  return result;
}

Status UpdateTxnOperation::Prepare() {
  return Status::OK();
}

void UpdateTxnOperation::Start() {
  if (!state()->has_hybrid_time()) {
    state()->set_hybrid_time(state()->tablet_peer()->clock().Now());
  }
}

TransactionCoordinator& UpdateTxnOperation::transaction_coordinator() const {
  return *state()->tablet_peer()->tablet()->transaction_coordinator();
}

ProcessingMode UpdateTxnOperation::mode() const {
  return type() == consensus::LEADER ? ProcessingMode::LEADER : ProcessingMode::NON_LEADER;
}

Status UpdateTxnOperation::Apply(gscoped_ptr<consensus::CommitMsg>* commit_msg) {
  auto* state = this->state();
  TransactionCoordinator::ReplicatedData data = {
      mode(),
      state->tablet_peer()->tablet(),
      *state->request(),
      state->op_id(),
      state->hybrid_time()
  };
  return transaction_coordinator().ProcessReplicated(data);
}

string UpdateTxnOperation::ToString() const {
  return Format("UpdateTxnOperation [state=$0]", state()->ToString());
}

void UpdateTxnOperation::Finish(OperationResult result) {
  if (result == OperationResult::ABORTED) {
    LOG(INFO) << "Aborted: " << state()->request()->ShortDebugString();
  }
}

} // namespace tablet
} // namespace yb
