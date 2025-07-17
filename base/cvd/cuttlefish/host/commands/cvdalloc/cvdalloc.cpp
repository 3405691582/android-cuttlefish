/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <android-base/logging.h>

#include "cuttlefish/common/libs/fs/shared_fd.h"

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"

ABSL_FLAG(std::string, socket, "", "Socket");
ABSL_FLAG(std::string, config, "", "Configuration file");

int main(int argc, char *argv[]) {
  std::vector<char *> args = absl::ParseCommandLine(argc, argv);

  LOG(INFO) << "cvdalloc: acquire";

  pid_t pid = fork();
  if (pid < 0) return -1;

  if (pid == 0) { // child

    LOG(INFO) << "cvdalloc2: waiting to release";

    auto sock = cuttlefish::SharedFD::Dup(std::stoi(absl::GetFlag(FLAGS_socket)));
    if (!sock->IsOpen()) {
      LOG(ERROR) << "cvdalloc2: Socket is closed: " << sock->StrError();
      return 1;
    }

    char buf[64] = {0};
    if (sock->Read(buf, sizeof(buf)) < 0) {
      sock->Close();
      return 1;
    }

    LOG(INFO) << "cvdalloc2: release";
    return 0;
  }

  LOG(INFO) << "cvdalloc: done";

  return 0;
}
