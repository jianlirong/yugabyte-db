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

#ifndef YB_CLIENT_TABLET_RPC_H
#define YB_CLIENT_TABLET_RPC_H

#include <unordered_set>

#include "yb/client/client.h"
#include "yb/client/client-internal.h"
#include "yb/client/client_fwd.h"

#include "yb/rpc/rpc_fwd.h"
#include "yb/rpc/rpc.h"

#include "yb/tserver/tserver.pb.h"

#include "yb/util/result.h"
#include "yb/util/status.h"
#include "yb/util/trace.h"

namespace yb {

namespace tserver {
class TabletServerServiceProxy;
}

namespace client {
namespace internal {

class TabletRpc {
 public:
  virtual const tserver::TabletServerErrorPB* response_error() const = 0;
  virtual void Failed(const Status& status) = 0;
  virtual void SendRpcToTserver() = 0;
 protected:
  ~TabletRpc() {}
};

class TabletInvoker {
 public:
  explicit TabletInvoker(bool consistent_prefix,
                         YBClient* client,
                         rpc::RpcCommand* command,
                         TabletRpc* rpc,
                         RemoteTablet* tablet,
                         rpc::RpcRetrier* retrier,
                         Trace* trace)
      : client_(client),
        command_(command),
        rpc_(rpc),
        tablet_(tablet),
        tablet_id_(tablet != nullptr ? tablet->tablet_id() : std::string()),
        retrier_(retrier),
        trace_(trace),
        consistent_prefix_(consistent_prefix) {}

  virtual ~TabletInvoker() {}

  void Execute(const std::string& tablet_id);
  bool Done(Status* status);

  bool IsLocalCall() const;
  const RemoteTabletPtr& tablet() const { return tablet_; }
  std::shared_ptr<tserver::TabletServerServiceProxy> proxy() const;
  YBClient& client() const { return *client_; }

 private:
  void SelectTabletServer();

  // This is an implementation of ReadRpc with consistency level as CONSISTENT_PREFIX. As a result,
  // there is no requirement that the read needs to hit the leader.
  void SelectTabletServerWithConsistentPrefix();

  // Called when we finish initializing a TS proxy.
  // Sends the RPC, provided there was no error.
  void InitTSProxyCb(const Status& status);

  // Marks all replicas on current_ts_ as failed and retries the write on a
  // new replica.
  void FailToNewReplica(const Status& reason);

  // Called when we finish a lookup (to find the new consensus leader). Retries
  // the rpc after a short delay.
  void LookupTabletCb(const Status& status);

  void InitialLookupTabletDone(const Status& status);

  YBClient* client_;

  rpc::RpcCommand* const command_;

  TabletRpc* const rpc_;

  // The tablet that should receive this rpc.
  RemoteTabletPtr tablet_;

  std::string tablet_id_;

  rpc::RpcRetrier* const retrier_;

  // Trace is provided externally and owner of this object should guarantee that it will be alive
  // while this object is alive.
  Trace* const trace_;

  // Used to retry some failed RPCs.
  // Tablet servers that refused the write because they were followers at the time.
  // Cleared when new consensus configuration information arrives from the master.
  std::unordered_set<RemoteTabletServer*> followers_;

  bool consistent_prefix_;

  // The TS receiving the write. May change if the write is retried.
  // RemoteTabletServer is taken from YBClient cache, so it is guaranteed that those objects are
  // alive while YBClient is alive. Because we don't delete them, but only add and update.
  RemoteTabletServer* current_ts_ = nullptr;
};

CHECKED_STATUS ErrorStatus(const tserver::TabletServerErrorPB* error);
tserver::TabletServerErrorPB_Code ErrorCode(const tserver::TabletServerErrorPB* error);

template <class Response>
HybridTime GetPropagatedHybridTime(const Response& response) {
  return response.has_propagated_hybrid_time() ? HybridTime(response.propagated_hybrid_time())
                                               : HybridTime::kInvalidHybridTime;
}

} // namespace internal
} // namespace client
} // namespace yb

#endif // YB_CLIENT_TABLET_RPC_H
