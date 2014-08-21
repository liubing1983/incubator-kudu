// Copyright (c) 2013, Cloudera, inc.

#include "kudu/tserver/tablet_server_options.h"

#include <glog/logging.h>
#include <gflags/gflags.h>

#include "kudu/master/master.h"
#include "kudu/tserver/tablet_server.h"
#include "kudu/util/env.h"

namespace kudu {
namespace tserver {

DEFINE_string(tablet_server_base_dir, "/tmp/demo-tablets",
              "Base directory for single-tablet demo server");

DEFINE_string(tablet_server_rpc_bind_addresses, "0.0.0.0:7050",
             "Comma-separated list of addresses for the Tablet Server"
              " to bind to for RPC connections");
DEFINE_int32(tablet_server_web_port, TabletServer::kDefaultWebPort,
             "Port to bind to for the tablet server web server");
DEFINE_int32(tablet_server_num_acceptors_per_address, 1,
             "Number of RPC acceptor threads for each bound address");
DEFINE_int32(tablet_server_num_service_threads, 10,
             "Number of RPC worker threads to run");
DEFINE_string(tablet_server_master_addr, "127.0.0.1:7051",
              "Address of the master which the tablet server should connect to. "
              "The master does not read this flag -- configure the master "
              "separately using 'master_server_rpc_bind_addresses'.");


TabletServerOptions::TabletServerOptions() {
  rpc_opts.rpc_bind_addresses = FLAGS_tablet_server_rpc_bind_addresses;
  rpc_opts.num_acceptors_per_address = FLAGS_tablet_server_num_acceptors_per_address;
  rpc_opts.num_service_threads = FLAGS_tablet_server_num_service_threads;
  rpc_opts.default_port = TabletServer::kDefaultPort;

  webserver_opts.port = FLAGS_tablet_server_web_port;
  // The rest of the web server options are not overridable on a per-tablet-server
  // basis.

  base_dir = FLAGS_tablet_server_base_dir;

  Status s = master_hostport.ParseString(FLAGS_tablet_server_master_addr,
                                         master::Master::kDefaultPort);
  if (!s.ok()) {
    LOG(FATAL) << "Couldn't parse tablet_server_master_addr flag: " << s.ToString();
  }

  env = Env::Default();
}

} // namespace tserver
} // namespace kudu