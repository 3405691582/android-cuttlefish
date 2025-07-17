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
#include "cuttlefish/host/libs/feature/command_source.h"

namespace cuttlefish {
namespace {

class Cvdalloc : public CommandSource {
 public:
  INJECT(Cvdalloc(const CuttlefishConfig &config,
                  const CuttlefishConfig::InstanceSpecific &instance))
      : config_(config), instance_(instance) {}

  // CommandSource
  Result<std::vector<MonitorCommand>> Commands() override {
    /*
     * For exit signaling, our end of the socket will be closed
     * implicitly when the run_cvd process exits.
     */
    auto cmd = Command(CvdallocBinary());
    cmd.AddParameter("--socket=", their_socket_);
    std::vector<MonitorCommand> commands;
    commands.emplace_back(std::move(cmd));
    return commands;
  }

  std::string Name() const override { return "Cvdalloc"; }
  std::unordered_set<SetupFeature *> Dependencies() const override {
    return {};
  }

 private:
  Result<void> ResultSetup() override {
    bool r = SharedFD::SocketPair(
        AF_LOCAL, SOCK_STREAM, 0, &socket_, &their_socket_);
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
      .addMultibinding<CommandSource, Cvdalloc>()
      .addMultibinding<SetupFeature, Cvdalloc>();
}

}  // namespace cuttlefish
