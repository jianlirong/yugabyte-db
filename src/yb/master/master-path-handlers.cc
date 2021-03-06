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

#include "yb/master/master-path-handlers.h"

#include <algorithm>
#include <functional>
#include <map>

#include "yb/common/partition.h"
#include "yb/common/schema.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/gutil/map-util.h"
#include "yb/gutil/stringprintf.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/master/catalog_manager.h"
#include "yb/master/master.h"
#include "yb/master/master.pb.h"
#include "yb/master/sys_catalog.h"
#include "yb/master/ts_descriptor.h"
#include "yb/master/ts_manager.h"
#include "yb/server/webui_util.h"
#include "yb/util/curl_util.h"
#include "yb/util/string_case.h"
#include "yb/util/url-coding.h"
#include "yb/util/version_info.h"
#include "yb/util/version_info.pb.h"

namespace yb {

using consensus::RaftPeerPB;
using std::vector;
using std::map;
using std::string;
using std::stringstream;
using std::unique_ptr;
using strings::Substitute;

using namespace std::placeholders;

namespace master {

MasterPathHandlers::~MasterPathHandlers() {
}

void MasterPathHandlers::CallIfLeaderOrPrintRedirect(
    const Webserver::WebRequest& req, stringstream* output,
    const Webserver::PathHandlerCallback& callback) {
  // Lock the CatalogManager in a self-contained block, to prevent double-locking on callbacks.
  {
    CatalogManager::ScopedLeaderSharedLock l(master_->catalog_manager());
    if (!l.first_failed_status().ok()) {
      *output << "<h1>This is not the Master Leader!</h1>\n";

      do {
        vector<ServerEntryPB> masters;
        Status s = master_->ListMasters(&masters);
        if (!s.ok()) {
          break;
        }

        string redirect;
        for (const ServerEntryPB& master : masters) {
          if (master.has_error()) {
            // This will leave redirect empty and thus fail accordingly.
            break;
          }

          if (master.role() == consensus::RaftPeerPB::LEADER) {
            // URI already starts with a /, so none is needed between $1 and $2.
            redirect = Substitute(
                "<a class=\"alert-link\" href=\"http://$0:$1$2$3\">Leader</a>",
                master.registration().http_addresses(0).host(),
                master.registration().http_addresses(0).port(), req.redirect_uri,
                req.query_string.empty() ? "" : "?" + req.query_string);
          }
        }

        if (redirect.empty()) {
          break;
        }

        *output << "<h3><div class=\"alert alert-warning\">"
                << "Please click  " << redirect << " to get redirected to the Master Leader!"
                << "</div></h3>";

        return;
      } while (0);

      *output << "Cannot get Leader information to help you redirect...\n";
      return;
    }
  }
  callback(req, output);
}

void MasterPathHandlers::HandleTabletServers(const Webserver::WebRequest& req,
                                             stringstream* output) {
  vector<std::shared_ptr<TSDescriptor> > descs;
  master_->ts_manager()->GetAllDescriptors(&descs);

  *output << "<h2>Tablet Servers</h2>\n";

  *output << "<table class='table table-striped'>\n";
  *output << "  <tr>\n"
          << "    <th>Server</th>\n"
          << "    <th>Time since heartbeat</th>\n"
          << "    <th>Load (Num Tablets)</th>\n"
          << "    <th>Cloud</th>\n"
          << "    <th>Region</th>\n"
          << "    <th>Zone</th>\n"
          << "    <th>TServer UUID</th>\n"
          << "  </tr>\n";
  for (const std::shared_ptr<TSDescriptor>& desc : descs) {
    const string time_since_hb = StringPrintf("%.1fs", desc->TimeSinceHeartbeat().ToSeconds());
    TSRegistrationPB reg;
    desc->GetRegistration(&reg);
    string host_port = Substitute("$0:$1",
                                  reg.common().rpc_addresses(0).host(),
                                  reg.common().rpc_addresses(0).port());
    *output << "  <tr>\n";
    *output << "    <td>" << RegistrationToHtml(reg.common(), host_port) << "</td>";
    *output << "    <td>" << time_since_hb << "</td>";
    *output << "    <td>" << desc->num_live_replicas() << "</td>";
    *output << "    <td>" << reg.common().cloud_info().placement_cloud() << "</td>";
    *output << "    <td>" << reg.common().cloud_info().placement_region() << "</td>";
    *output << "    <td>" << reg.common().cloud_info().placement_zone() << "</td>";
    *output << "    <td>" << desc->permanent_uuid() << "</td>";
    *output << "  </tr>\n";
  }
  *output << "</table>\n";
}

void MasterPathHandlers::HandleCatalogManager(const Webserver::WebRequest& req,
                                              stringstream* output,
                                              bool skip_system_tables) {
  vector<scoped_refptr<TableInfo> > tables;
  master_->catalog_manager()->GetAllTables(&tables);
  string title = skip_system_tables ? "User Tables" : "All Tables";

  (*output) << "<div class='panel panel-default'>\n"
            << "<div class='panel-heading'><h2 class='panel-title'>" << title << "</h2></div>\n";
  (*output) << "<div class='panel-body'>";
  typedef map<string, string> StringMap;
  StringMap ordered_tables;
  for (const scoped_refptr<TableInfo>& table : tables) {
    auto l = table->LockForRead();
    if (!l->data().is_running()) {
      continue;
    }

    // Skip system tables if we should.
    if (skip_system_tables && master_->catalog_manager()->IsSystemTable(*table)) {
      continue;
    }

    const TableName long_table_name = TableLongName(
        master_->catalog_manager()->GetNamespaceName(table->namespace_id()), l->data().name());
    string keyspace = master_->catalog_manager()->GetNamespaceName(table->namespace_id());
    string state = SysTablesEntryPB_State_Name(l->data().pb.state());
    Capitalize(&state);
    ordered_tables[long_table_name] = Substitute(
        "<tr><td>$0</td><td><a href=\"/table?id=$3\">$1</a></td><td>$2</td><td>$3 $4</td></tr>\n",
        EscapeForHtmlToString(keyspace),
        EscapeForHtmlToString(l->data().name()),
        state,
        EscapeForHtmlToString(table->id()),
        EscapeForHtmlToString(l->data().pb.state_msg()));
  }
  if (ordered_tables.size() == 0) {
    (*output) << "You do not have any tables.";
  } else {
    *output << "<table class='table table-striped'>\n";
    *output << "  <tr><th>Keyspace</th><th>Table Name</th><th>State</th><th>UUID</th></tr>\n";
    for (const StringMap::value_type &table : ordered_tables) {
      *output << table.second;
    }
    (*output) << "</table>\n";
  }
  (*output) << "</div> <!-- panel-body -->\n";
  (*output) << "</div> <!-- panel -->\n";
}

namespace {

bool CompareByRole(const TabletReplica& a, const TabletReplica& b) {
  return a.role < b.role;
}

} // anonymous namespace


void MasterPathHandlers::HandleTablePage(const Webserver::WebRequest& req,
                                         stringstream *output) {
  // Parse argument.
  string table_id;
  if (!FindCopy(req.parsed_args, "id", &table_id)) {
    // TODO: webserver should give a way to return a non-200 response code
    *output << "Missing 'id' argument";
    return;
  }

  CatalogManager::ScopedLeaderSharedLock l(master_->catalog_manager());
  if (!l.first_failed_status().ok()) {
    *output << "Master is not ready: " << l.first_failed_status().ToString();
    return;
  }

  scoped_refptr<TableInfo> table = master_->catalog_manager()->GetTableInfo(table_id);
  if (table == nullptr) {
    *output << "Table not found";
    return;
  }

  Schema schema;
  PartitionSchema partition_schema;
  NamespaceName keyspace_name;
  TableName table_name;
  vector<scoped_refptr<TabletInfo> > tablets;
  {
    auto l = table->LockForRead();
    keyspace_name = master_->catalog_manager()->GetNamespaceName(table->namespace_id());
    table_name = l->data().name();
    *output << "<h1>Table: " << EscapeForHtmlToString(TableLongName(keyspace_name, table_name))
            << " (" << EscapeForHtmlToString(table_id) << ")</h1>\n";

    *output << "<table class='table table-striped'>\n";
    *output << "  <tr><td>Version:</td><td>" << l->data().pb.version() << "</td></tr>\n";

    *output << "  <tr><td>Type:</td><td>" << TableType_Name(l->data().pb.table_type())
            << "</td></tr>\n";

    string state = SysTablesEntryPB_State_Name(l->data().pb.state());
    Capitalize(&state);
    *output << "  <tr><td>State:</td><td>"
            << state
            << EscapeForHtmlToString(l->data().pb.state_msg())
            << "</td></tr>\n";
    *output << "</table>\n";

    SchemaFromPB(l->data().pb.schema(), &schema);
    Status s = PartitionSchema::FromPB(l->data().pb.partition_schema(), schema, &partition_schema);
    if (!s.ok()) {
      *output << "Unable to decode partition schema: " << s.ToString();
      return;
    }
    table->GetAllTablets(&tablets);
  }

  HtmlOutputSchemaTable(schema, output);

  *output << "<table class='table table-striped'>\n";
  *output << "  <tr><th>Tablet ID</th><th>Partition</th><th>State</th>"
      "<th>Message</th><th>RaftConfig</th></tr>\n";
  for (const scoped_refptr<TabletInfo>& tablet : tablets) {
    TabletInfo::ReplicaMap locations;
    tablet->GetReplicaLocations(&locations);
    vector<TabletReplica> sorted_locations;
    AppendValuesFromMap(locations, &sorted_locations);
    std::sort(sorted_locations.begin(), sorted_locations.end(), &CompareByRole);

    auto l = tablet->LockForRead();

    Partition partition;
    Partition::FromPB(l->data().pb.partition(), &partition);

    string state = SysTabletsEntryPB_State_Name(l->data().pb.state());
    Capitalize(&state);
    *output << Substitute(
        "<tr><th>$0</th><td>$1</td><td>$2</td><td>$3</td><td>$4</td></tr>\n",
        tablet->tablet_id(),
        EscapeForHtmlToString(partition_schema.PartitionDebugString(partition, schema)),
        state,
        EscapeForHtmlToString(l->data().pb.state_msg()),
        RaftConfigToHtml(sorted_locations, tablet->tablet_id()));
  }
  *output << "</table>\n";

  HtmlOutputTasks(table->GetTasks(), output);
}

void MasterPathHandlers::RootHandler(const Webserver::WebRequest& req,
                                     stringstream* output) {

  // First check if we are the master leader. If not, make a curl call to the master leader and
  // return that as the UI payload.
  vector<ServerEntryPB> masters;
  CatalogManager::ScopedLeaderSharedLock l(master_->catalog_manager());
  if (!l.first_failed_status().ok()) {
    do {
      // List all the masters.
      Status s = master_->ListMasters(&masters);
      if (!s.ok()) {
        s = s.CloneAndPrepend("Unable to list Masters");
        LOG(WARNING) << s.ToString();
        *output << "<h2>" << s.ToString() << "</h2>\n";
        return;
      }

      // Find the URL of the current master leader.
      string redirect;
      for (const ServerEntryPB& master : masters) {
        if (master.has_error()) {
          // This will leave redirect empty and thus fail accordingly.
          break;
        }

        if (master.role() == consensus::RaftPeerPB::LEADER) {
          // URI already starts with a /, so none is needed between $1 and $2.
          redirect = Substitute("http://$0:$1$2$3",
                                master.registration().http_addresses(0).host(),
                                master.registration().http_addresses(0).port(),
                                req.redirect_uri,
                                req.query_string.empty() ? "?raw" :
                                                           "?" + req.query_string + "&raw");
        }
      }
      // Fail if we were not able to find the current master leader.
      if (redirect.empty()) {
        break;
      }
      // Make a curl call to the current master leader and return that payload as the result of the
      // web request.
      EasyCurl curl;
      faststring buf;
      s = curl.FetchURL(redirect, &buf);
      if (s.ok()) {
        *output << buf.ToString();
        return;
      }
    } while (0);

    *output << "Cannot get Leader information to help you redirect...\n";
    return;
  }

  SysClusterConfigEntryPB config;
  Status s = master_->catalog_manager()->GetClusterConfig(&config);
  if (!s.ok()) {
    *output << "<div class=\"alert alert-warning\">" << s.ToString() << "</div>";
    return;
  }

  // Get all the tables.
  vector<scoped_refptr<TableInfo> > tables;
  master_->catalog_manager()->GetAllTables(&tables);

  // Get the list of user tables.
  vector<scoped_refptr<TableInfo> > user_tables;
  for (scoped_refptr<TableInfo> table : tables) {
    if (!master_->catalog_manager()->IsSystemTable(*table)) {
      user_tables.push_back(table);
    }
  }
  // Get the version info.
  VersionInfoPB version_info;
  VersionInfo::GetVersionInfoPB(&version_info);

  // Display the overview information.
  (*output) << "<h1>YugaByte DB</h1>\n";

  (*output) << "<div class='row dashboard-content'>\n";

  (*output) << "<div class='col-xs-12 col-md-8 col-lg-6'>\n";
  (*output) << "<div class='panel panel-default'>\n"
            << "<div class='panel-heading'><h2 class='panel-title'> Overview</h2></div>\n";
  (*output) << "<div class='panel-body'>";
  (*output) << "<table class='table'>\n";
  (*output) << Substitute("  <tr><td>$0<span class='yb-overview'>$1</span></td><td>$2</td></tr>\n",
                          "<i class='fa fa-database yb-dashboard-icon' aria-hidden='true'></i>",
                          "Universe UUID ",
                          config.cluster_uuid());

  // Replication factor.
  (*output) << "  <tr>";
  (*output) << Substitute(" <td>$0<span class='yb-overview'>$1</span></td>",
                          "<i class='fa fa-files-o yb-dashboard-icon' aria-hidden='true'></i>",
                          "Replication Factor ");
  (*output) << Substitute(" <td>$0</td>", master_->opts().GetMasterAddresses().get()->size());
  (*output) << "  </tr>\n";

  // Tserver count.
  (*output) << "  <tr>";
  (*output) << Substitute(" <td>$0<span class='yb-overview'>$1</span></td>",
                          "<i class='fa fa-server yb-dashboard-icon' aria-hidden='true'></i>",
                          "Num Nodes (TServers) ");
  (*output) << Substitute(" <td>$0 <a href='$1' class='btn btn-default pull-right'>$2</a></td>",
                          master_->ts_manager()->GetCount(),
                          "/tablet-servers",
                          "See all nodes &raquo;");
  (*output) << "  </tr>\n";

  // Num user tables.
  (*output) << "  <tr>";
  (*output) << Substitute(" <tr><td>$0<span class='yb-overview'>$1</span></td>",
                          "<i class='fa fa-table yb-dashboard-icon' aria-hidden='true'></i>",
                          "Num User Tables ");
  (*output) << Substitute(" <td>$0 <a href='$1' class='btn btn-default pull-right'>$2</a></td>",
                          user_tables.size(),
                          "/tables",
                          "See all tables &raquo;");
  (*output) << "  </tr>\n";

  // Build version and type.
  (*output) << Substitute("  <tr><td>$0<span class='yb-overview'>$1</span></td><td>$2</td></tr>\n",
                          "<i class='fa fa-code-fork yb-dashboard-icon' aria-hidden='true'></i>",
                          "YugaByte Version ", version_info.version_number());
  (*output) << Substitute("  <tr><td>$0<span class='yb-overview'>$1</span></td><td>$2</td></tr>\n",
                          "<i class='fa fa-terminal yb-dashboard-icon' aria-hidden='true'></i>",
                          "Build Type ", version_info.build_type());
  (*output) << "</table>";
  (*output) << "</div> <!-- panel-body -->\n";
  (*output) << "</div> <!-- panel -->\n";
  (*output) << "</div> <!-- col-xs-12 col-md-8 col-lg-6 -->\n";

  // Display the master info.
  (*output) << "<div class='col-xs-12 col-md-8 col-lg-6'>\n";
  HandleMasters(req, output);
  (*output) << "</div> <!-- col-xs-12 col-md-8 col-lg-6 -->\n";

  // Display the user tables if any.
  (*output) << "<div class='col-md-12 col-lg-12'>\n";
  HandleCatalogManager(req, output, true /* skip_system_tables */);
  (*output) << "</div> <!-- col-md-12 col-lg-12 -->\n";

  // Display the tablet server info.
  (*output) << "<div class='col-md-12 col-lg-12'>\n";
  HandleTabletServers(req, output);
  (*output) << "</div> <!-- col-md-12 col-lg-12 -->\n";

  (*output) << "</div> <!-- row dashboard-content -->\n";
}

void MasterPathHandlers::HandleMasters(const Webserver::WebRequest& req,
                                       stringstream* output) {
  vector<ServerEntryPB> masters;
  Status s = master_->ListMasters(&masters);
  if (!s.ok()) {
    s = s.CloneAndPrepend("Unable to list Masters");
    LOG(WARNING) << s.ToString();
    *output << "<h1>" << s.ToString() << "</h1>\n";
    return;
  }
  (*output) << "<div class='panel panel-default'>\n"
            << "<div class='panel-heading'><h2 class='panel-title'>Masters</h2></div>\n";
  (*output) << "<div class='panel-body'>";
  (*output) << "<table class='table'>\n";
  (*output) << "  <tr>\n"
            << "    <th>Server</th>\n"
            << "    <th>RAFT Role</th>"
            << "    <th>Details</th>\n"
            << "  </tr>\n";

  for (const ServerEntryPB& master : masters) {
    if (master.has_error()) {
      Status error = StatusFromPB(master.error());
      *output << "  <tr>\n";
      *output << Substitute("    <td colspan=2><font color='red'><b>$0</b></font></td>\n",
                            EscapeForHtmlToString(error.ToString()));
      *output << "  </tr>\n";
      continue;
    }
    string host_port = Substitute("$0:$1",
                                  master.registration().rpc_addresses(0).host(),
                                  master.registration().rpc_addresses(0).port());
    string reg_text = RegistrationToHtml(master.registration(), host_port);
    if (master.instance_id().permanent_uuid() == master_->instance_pb().permanent_uuid()) {
      reg_text = Substitute("<b>$0</b>", reg_text);
    }
    string raft_role = master.has_role() ?  RaftPeerPB_Role_Name(master.role()) : "N/A";
    string cloud = master.registration().cloud_info().placement_cloud();
    string region = master.registration().cloud_info().placement_region();
    string zone = master.registration().cloud_info().placement_zone();

    *output << "  <tr>\n"
            << "    <td>" << reg_text << "</td>\n"
            << "    <td>" << raft_role << "</td>\n"
            << "    <td><div><span class='yb-overview'>CLOUD: </span>" << cloud << "</div>\n"
            << "        <div><span class='yb-overview'>REGION: </span>" << region << "</div>\n"
            << "        <div><span class='yb-overview'>ZONE: </span>" << zone << "</div>\n"
            << "        <div><span class='yb-overview'>UUID: </span>"
            << master.instance_id().permanent_uuid()
            << "</div></td>\n"
            << "  </tr>\n";
  }

  (*output) << "</table>";
  (*output) << "</div> <!-- panel-body -->\n";
  (*output) << "</div> <!-- panel -->\n";
}

namespace {

// Visitor for the catalog table which dumps tables and tablets in a JSON format. This
// dump is interpreted by the CM agent in order to track time series entities in the SMON
// database.
//
// This implementation relies on scanning the catalog table directly instead of using the
// catalog manager APIs. This allows it to work even on a non-leader master, and avoids
// any requirement for locking. For the purposes of metrics entity gathering, it's OK to
// serve a slightly stale snapshot.
//
// It is tempting to directly dump the metadata protobufs using JsonWriter::Protobuf(...),
// but then we would be tying ourselves to textual compatibility of the PB field names in
// our catalog table. Instead, the implementation specifically dumps the fields that we
// care about.
//
// This should be considered a "stable" protocol -- do not rename, remove, or restructure
// without consulting with the CM team.
class JsonDumperBase {
 public:
  explicit JsonDumperBase(JsonWriter* jw) : jw_(jw) {}

  virtual ~JsonDumperBase() {}

  virtual std::string name() const = 0;

 protected:
  JsonWriter* jw_;
};

class JsonKeyspaceDumper : public Visitor<PersistentNamespaceInfo>, public JsonDumperBase {
 public:
  explicit JsonKeyspaceDumper(JsonWriter* jw) : JsonDumperBase(jw) {}

  std::string name() const override { return "keyspaces"; }

  virtual Status Visit(const std::string& keyspace_id,
                       const SysNamespaceEntryPB& metadata) override {
    jw_->StartObject();
    jw_->String("keyspace_id");
    jw_->String(keyspace_id);

    jw_->String("keyspace_name");
    jw_->String(metadata.name());

    jw_->EndObject();
    return Status::OK();
  }
};

class JsonTableDumper : public Visitor<PersistentTableInfo>, public JsonDumperBase {
 public:
  explicit JsonTableDumper(JsonWriter* jw) : JsonDumperBase(jw) {}

  std::string name() const override { return "tables"; }

  Status Visit(const std::string& table_id, const SysTablesEntryPB& metadata) override {
    if (metadata.state() != SysTablesEntryPB::RUNNING) {
      return Status::OK();
    }

    jw_->StartObject();
    jw_->String("table_id");
    jw_->String(table_id);

    jw_->String("keyspace_id");
    jw_->String(metadata.namespace_id());

    jw_->String("table_name");
    jw_->String(metadata.name());

    jw_->String("state");
    jw_->String(SysTablesEntryPB::State_Name(metadata.state()));

    jw_->EndObject();
    return Status::OK();
  }
};

class JsonTabletDumper : public Visitor<PersistentTabletInfo>, public JsonDumperBase {
 public:
  explicit JsonTabletDumper(JsonWriter* jw) : JsonDumperBase(jw) {}

  std::string name() const override { return "tablets"; }

  Status Visit(const std::string& tablet_id, const SysTabletsEntryPB& metadata) override {
    const std::string& table_id = metadata.table_id();
    if (metadata.state() != SysTabletsEntryPB::RUNNING) {
      return Status::OK();
    }

    jw_->StartObject();
    jw_->String("table_id");
    jw_->String(table_id);

    jw_->String("tablet_id");
    jw_->String(tablet_id);

    jw_->String("state");
    jw_->String(SysTabletsEntryPB::State_Name(metadata.state()));

    // Dump replica UUIDs
    if (metadata.has_committed_consensus_state()) {
      const consensus::ConsensusStatePB& cs = metadata.committed_consensus_state();
      jw_->String("replicas");
      jw_->StartArray();
      for (const RaftPeerPB& peer : cs.config().peers()) {
        jw_->StartObject();
        jw_->String("type");
        jw_->String(RaftPeerPB::MemberType_Name(peer.member_type()));

        jw_->String("server_uuid");
        jw_->String(peer.permanent_uuid());

        jw_->String("addr");
        jw_->String(Substitute("$0:$1", peer.last_known_addr().host(),
                               peer.last_known_addr().port()));

        jw_->EndObject();
      }
      jw_->EndArray();

      if (cs.has_leader_uuid()) {
        jw_->String("leader");
        jw_->String(cs.leader_uuid());
      }
    }

    jw_->EndObject();
    return Status::OK();
  }
};

template <class Dumper>
Status JsonDumpCollection(JsonWriter* jw, Master* master, stringstream* output) {
  unique_ptr<Dumper> json_dumper(new Dumper(jw));
  jw->String(json_dumper->name());
  jw->StartArray();
  const Status s = master->catalog_manager()->sys_catalog()->Visit(json_dumper.get());
  if (s.ok()) {
    // End the array only if there is no error.
    jw->EndArray();
  } else {
    // Print just an error message.
    output->str("");
    JsonWriter jw_err(output, JsonWriter::COMPACT);
    jw_err.StartObject();
    jw_err.String("error");
    jw_err.String(s.ToString());
    jw_err.EndObject();
  }
  return s;
}

} // anonymous namespace

void MasterPathHandlers::HandleDumpEntities(const Webserver::WebRequest& req,
                                            stringstream* output) {
  JsonWriter jw(output, JsonWriter::COMPACT);
  jw.StartObject();

  if (JsonDumpCollection<JsonKeyspaceDumper>(&jw, master_, output).ok() &&
      JsonDumpCollection<JsonTableDumper>(&jw, master_, output).ok() &&
      JsonDumpCollection<JsonTabletDumper>(&jw, master_, output).ok()) {
    // End the object only if there is no error.
    jw.EndObject();
  }
}

void MasterPathHandlers::HandleGetClusterConfig(
    const Webserver::WebRequest& req, stringstream* output) {
  *output << "<h1>Current Cluster Config</h1>\n";
  SysClusterConfigEntryPB config;
  Status s = master_->catalog_manager()->GetClusterConfig(&config);
  if (!s.ok()) {
    *output << "<div class=\"alert alert-warning\">" << s.ToString() << "</div>";
    return;
  }

  *output << "<div class=\"alert alert-success\">Successfully got cluster config!</div>"
          << "<pre class=\"prettyprint\">" << config.DebugString() << "</pre>";
}

Status MasterPathHandlers::Register(Webserver* server) {
  bool is_styled = true;
  bool is_on_nav_bar = true;
  // Cannot use auto with callbacks, as they won't properly deduce types with boost magic...
  server->RegisterPathHandler(
    "/", "Home", std::bind(&MasterPathHandlers::RootHandler, this, _1, _2), is_styled,
    is_on_nav_bar);

  Webserver::PathHandlerCallback cb =
      std::bind(&MasterPathHandlers::HandleTabletServers, this, _1, _2);
  server->RegisterPathHandler(
      "/tablet-servers", "Tablet Servers",
      std::bind(&MasterPathHandlers::CallIfLeaderOrPrintRedirect, this, _1, _2, cb), is_styled,
      is_on_nav_bar);
  cb = std::bind(&MasterPathHandlers::HandleCatalogManager,
                 this, _1, _2, false /* skip_system_tables */);
  server->RegisterPathHandler(
      "/tables", "Tables",
      std::bind(&MasterPathHandlers::CallIfLeaderOrPrintRedirect, this, _1, _2, cb), is_styled,
      is_on_nav_bar);
  cb = std::bind(&MasterPathHandlers::HandleTablePage, this, _1, _2);
  server->RegisterPathHandler(
      "/table", "", std::bind(&MasterPathHandlers::CallIfLeaderOrPrintRedirect, this, _1, _2, cb),
      is_styled, false);
  server->RegisterPathHandler(
      "/masters", "Masters", std::bind(&MasterPathHandlers::HandleMasters, this, _1, _2), is_styled,
      is_on_nav_bar);
  cb = std::bind(&MasterPathHandlers::HandleDumpEntities, this, _1, _2);
  server->RegisterPathHandler(
      "/dump-entities", "Dump Entities",
      std::bind(&MasterPathHandlers::CallIfLeaderOrPrintRedirect, this, _1, _2, cb), false, false);
  cb = std::bind(&MasterPathHandlers::HandleGetClusterConfig, this, _1, _2);
  server->RegisterPathHandler(
      "/cluster-config", "Cluster Config",
      std::bind(&MasterPathHandlers::CallIfLeaderOrPrintRedirect, this, _1, _2, cb), is_styled,
      is_on_nav_bar);
  return Status::OK();
}

string MasterPathHandlers::RaftConfigToHtml(const std::vector<TabletReplica>& locations,
                                            const std::string& tablet_id) const {
  stringstream html;

  html << "<ul>\n";
  for (const TabletReplica& location : locations) {
    string location_html = TSDescriptorToHtml(*location.ts_desc, tablet_id);
    if (location.role == RaftPeerPB::LEADER) {
      html << Substitute("  <li><b>LEADER: $0</b></li>\n", location_html);
    } else {
      html << Substitute("  <li>$0: $1</li>\n",
                         RaftPeerPB_Role_Name(location.role), location_html);
    }
  }
  html << "</ul>\n";
  return html.str();
}

string MasterPathHandlers::TSDescriptorToHtml(const TSDescriptor& desc,
                                              const std::string& tablet_id) const {
  TSRegistrationPB reg;
  desc.GetRegistration(&reg);

  if (reg.common().http_addresses().size() > 0) {
    return Substitute(
        "<a href=\"http://$0:$1/tablet?id=$2\">$3</a>", reg.common().http_addresses(0).host(),
        reg.common().http_addresses(0).port(), EscapeForHtmlToString(tablet_id),
        EscapeForHtmlToString(reg.common().http_addresses(0).host()));
  } else {
    return EscapeForHtmlToString(desc.permanent_uuid());
  }
}

string MasterPathHandlers::RegistrationToHtml(
    const ServerRegistrationPB& reg, const std::string& link_text) const {
  string link_html = EscapeForHtmlToString(link_text);
  if (reg.http_addresses().size() > 0) {
    link_html = Substitute("<a href=\"http://$0:$1/\">$2</a>",
                           reg.http_addresses(0).host(),
                           reg.http_addresses(0).port(), link_html);
  }
  return link_html;
}

} // namespace master
} // namespace yb
