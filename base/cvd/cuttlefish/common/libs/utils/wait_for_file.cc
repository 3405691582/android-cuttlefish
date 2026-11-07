/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "cuttlefish/common/libs/utils/wait_for_file.h"

#include <poll.h>

#include <chrono>
#include <string>
#include <vector>

#include "android-base/file.h"

#include "cuttlefish/common/libs/fs/shared_fd.h"
#include "cuttlefish/common/libs/utils/file_watch.h"
#include "cuttlefish/files/file_exists.h"
#include "cuttlefish/result/result.h"

namespace cuttlefish {

Result<void> WaitForFile(const std::string& path, int timeoutSec) {
  CF_EXPECT_NE(path, "", "Path is empty");

  if (FileExists(path, true)) {
    return {};
  }

  const auto targetTime =
      std::chrono::system_clock::now() + std::chrono::seconds(timeoutSec);

  const std::string parentPath = android::base::Dirname(path);

  CF_EXPECT(WaitForFile(parentPath, timeoutSec),
            "Error while waiting for parent directory creation");

  // Arm the watch before re-checking, so that a file created in between
  // is not missed.
  FileWatch watch =
      CF_EXPECT(FileWatch::Create(parentPath, FileWatch::kCreate));

  while (!FileExists(path, true)) {
    const auto currentTime = std::chrono::system_clock::now();

    if (currentTime >= targetTime) {
      return CF_ERR("Timed out");
    }

    const auto timeRemain =
        std::chrono::duration_cast<std::chrono::milliseconds>(targetTime -
                                                              currentTime)
            .count();
    std::vector<PollSharedFd> fds = {
        // NOLINTNEXTLINE(misc-include-cleaner): <poll.h>
        PollSharedFd{.fd = watch.ReadyFd(), .events = POLLIN, .revents = 0},
    };
    const int ret = SharedFD::Poll(fds, timeRemain);

    if (ret == 0) {
      return CF_ERR("Timed out");
    } else if (ret < 0) {
      return CF_ERRNO("poll() failed");
    }

    CF_EXPECT(watch.Drain());
  }

  return {};
}

}  // namespace cuttlefish
