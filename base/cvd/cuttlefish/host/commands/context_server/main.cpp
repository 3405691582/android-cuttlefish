// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

#include <gflags/gflags.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include "cuttlefish/host/commands/context_server/context.grpc.pb.h"

using contextserver::ContextReply;
using contextserver::ContextRequest;
using contextserver::ContextService;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

DEFINE_string(grpc_uds_path, "", "grpc_uds_path");

class ContextServiceImpl final : public ContextService::Service {
  Status GetContext(ServerContext* context, const ContextRequest* request,
                    ContextReply* reply) override {
    char hostname[1024];
    gethostname(hostname, 1024);
    reply->set_hostname(hostname);

    return Status::OK;
  }
};

void RunServer() {
  std::string server_address("unix:" + FLAGS_grpc_uds_path);
  ContextServiceImpl service;

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << std::endl;
  server->Wait();
}

int main(int argc, char** argv) {
  ::gflags::ParseCommandLineFlags(&argc, &argv, true);
  RunServer();

  return 0;
}
