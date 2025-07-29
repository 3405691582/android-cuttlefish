//
// Copyright (C) 2025 The Android Open Source Project
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

#include "cuttlefish/host/commands/run_cvd/launch/cvdalloc.h"

#include <android-base/logging.h>

#include "cuttlefish/common/libs/fs/shared_fd.h"
#include "cuttlefish/common/libs/utils/result.h"
#include "cuttlefish/common/libs/utils/subprocess.h"
#include "cuttlefish/host/libs/config/cuttlefish_config.h"
#include "cuttlefish/host/libs/config/known_paths.h"
#include "cuttlefish/host/libs/vm_manager/vm_manager.h"
#include "cuttlefish/host/libs/command_util/util.h"

namespace cuttlefish {
namespace {

constexpr int kCvdAllocateTimeout = 30;
constexpr int kCvdTeardownTimeout = 2;

Result<void> Post(const SharedFD &socket) {
  int i = 0;
  CF_EXPECT(socket->Write(&i, sizeof(int)) != -1);
  return {};
}

Result<void> Wait(const SharedFD &socket, int timeout) {
  CF_EXPECT(WaitForRead(socket, timeout), "WaitForRead");
  int i = -1;
  int r = socket->Read(&i, sizeof(i));
  CF_EXPECT(r >= 0, "Read");
  if (r > 0) CF_EXPECT(i == 0, "Unexpected read");
  return {};
}

class Cvdalloc : public vm_manager::VmmDependencyCommand {
 public:
  INJECT(Cvdalloc(const CuttlefishConfig &config,
                  const CuttlefishConfig::InstanceSpecific &instance))
      : config_(config), instance_(instance) {}

  // CommandSource
  Result<std::vector<MonitorCommand>> Commands() override {
    auto nice_stop = [this]() {
       LOG(INFO) << "cvdalloc (run_cvd): stop requested";
       if (!Post(socket_).ok()) {
         return StopperResult::kStopFailure;
       }

       bool wait_ok = Wait(socket_, kCvdTeardownTimeout).ok();
       LOG(INFO) << "cvdalloc (run_cvd): teardown completed";
       return wait_ok? StopperResult::kStopSuccess : StopperResult::kStopFailure;
    };
    auto cmd = Command(CvdallocBinary(), KillSubprocessFallback(nice_stop));
    cmd.AddParameter("--id=", instance_.id());
    cmd.AddParameter("--socket=", their_socket_);

    std::vector<MonitorCommand> commands;
    commands.emplace_back(std::move(cmd));
    return commands;
  }

  std::string Name() const override { return "Cvdalloc"; }
  bool Enabled() const override { return instance_.use_cvdalloc(); }
  std::unordered_set<SetupFeature *> Dependencies() const override {
    return {};
  }

  // StatusCheckCommandSource
  Result<void> WaitForAvailability() const override {
    LOG(INFO) << "cvdalloc (run_cvd): waiting for cvdalloc to allocate.";
    CF_EXPECT(Wait(socket_, kCvdAllocateTimeout));
    LOG(INFO) << "cvdalloc (run_cvd): allocation is done.";

    return {};
  }

 private:
  Result<void> ResultSetup() override {
    bool r = SharedFD::SocketPair(AF_LOCAL, SOCK_STREAM, 0, &socket_,
                                  &their_socket_);
    CF_EXPECT(r == true, "Unable to create socketpair: " << strerror(errno));

    return {};
  }

  const CuttlefishConfig &config_;
  const CuttlefishConfig::InstanceSpecific &instance_;
  SharedFD socket_, their_socket_;
};

}  // namespace

fruit::Component<fruit::Required<const CuttlefishConfig,
                                 const CuttlefishConfig::InstanceSpecific>>
CvdallocComponent() {
  return fruit::createComponent()
      .addMultibinding<vm_manager::VmmDependencyCommand, Cvdalloc>()
      .addMultibinding<CommandSource, Cvdalloc>()
      .addMultibinding<SetupFeature, Cvdalloc>();
}

}  // namespace cuttlefish
