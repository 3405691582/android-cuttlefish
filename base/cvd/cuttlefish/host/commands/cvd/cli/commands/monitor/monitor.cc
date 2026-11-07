/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "cuttlefish/host/commands/cvd/cli/commands/monitor/monitor.h"

#include <stdint.h>
#include <stdio.h>
#include <sys/poll.h>
#include <unistd.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/strings/str_cat.h"

#include "cuttlefish/ansi_codes/ansi_codes.h"
#include "cuttlefish/common/libs/fs/shared_fd.h"
#include "cuttlefish/common/libs/utils/file_watch.h"
#include "cuttlefish/common/libs/utils/tee_logging.h"
#include "cuttlefish/host/commands/cvd/cli/commands/monitor/display.h"
#include "cuttlefish/host/commands/cvd/cli/commands/monitor/log_sources.h"
#include "cuttlefish/host/commands/cvd/cli/commands/monitor/monitor_source.h"
#include "cuttlefish/host/commands/cvd/cli/utils.h"
#include "cuttlefish/host/commands/cvd/instances/local_instance.h"
#include "cuttlefish/posix/strerror.h"
#include "cuttlefish/result/expect.h"
#include "cuttlefish/result/result_type.h"

namespace cuttlefish {

Result<void> MonitorLogs(const LocalInstance& instance, SharedFD stop_fd,
                         LogSeverity severity) {
  CF_EXPECT(isatty(0), "The monitor command requires an interactive terminal.");

  std::unique_ptr<MonitorSource> kernel_monitor_source;
  std::unique_ptr<MonitorSource> launcher_monitor_source;
  std::unique_ptr<MonitorSource> logcat_monitor_source;

  const std::string logs_dir =
      absl::StrCat(instance.InstanceDirectory(), "/logs");
  // The logs directory may not exist yet while the device is starting;
  // keep trying until it does.
  std::optional<FileWatch> dir_watch;

  std::chrono::steady_clock::time_point last_draw_time =
      std::chrono::steady_clock::time_point();

  std::cout << kXtermUseAlternateScreen;
  std::cout.flush();
  absl::Cleanup clean_terminal = [] {
    std::cout << kAnsiReset << kAnsiClearScreen << kXtermUseMainScreen;
    std::cout.flush();
  };

  while (true) {
    if (!dir_watch.has_value()) {
      Result<FileWatch> watch =
          FileWatch::Create(logs_dir, FileWatch::kCreate | FileWatch::kRemove);
      if (watch.has_value()) {
        dir_watch = std::move(*watch);
      }
    }
    if (!kernel_monitor_source.get()) {
      kernel_monitor_source = KernelLogMonitorSource(instance);
    }
    if (!logcat_monitor_source.get()) {
      logcat_monitor_source = LogcatMonitorSource(instance, severity);
    }
    if (!launcher_monitor_source.get()) {
      launcher_monitor_source = LauncherLogMonitorSource(instance, severity);
    }

    TerminalSize term_size =
        GetTerminalSize().value_or(TerminalSize{.rows = 35, .columns = 80});
    term_size.rows -= 3;
    term_size.columns -= 1;
    LogMonitorDisplay display(term_size.columns);

    bool missing_source = false;
    const std::vector<MonitorSource*> sources = {
        launcher_monitor_source.get(),
        kernel_monitor_source.get(),
        logcat_monitor_source.get(),
    };

    for (size_t i = 0; i < sources.size(); i++) {
      MonitorSource* source = sources[i];
      if (source == nullptr) {
        missing_source = true;
      }
      size_t lines = (term_size.rows / 3) - 1;
      while (i + 1 < sources.size() && sources[i + 1] == nullptr) {
        lines += (term_size.rows / 3) - 1;
        i++;
        missing_source = true;
      }
      display.DrawReport(source, lines);
    }

    const auto [output, total_lines_drawn] = display.Finalize();
    std::cout << kAnsiClearScreen << kAnsiCursorTopLeft << output << std::flush;

    // Enforce a maximum framerate (max 20 FPS / min 50ms between draws)
    // so we don't saturate SSH bandwidth or CPU during heavy, continuous
    // logging.
    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    const std::chrono::steady_clock::duration elapsed = now - last_draw_time;
    constexpr std::chrono::milliseconds min_frame_time(50);
    if (elapsed < min_frame_time) {
      std::this_thread::sleep_for(min_frame_time - elapsed);
    }
    last_draw_time = std::chrono::steady_clock::now();

    // Setup poll_fds
    std::vector<PollSharedFd> poll_fds;
    if (stop_fd->IsOpen()) {
      poll_fds.push_back(
          PollSharedFd{.fd = stop_fd, .events = POLLIN, .revents = 0});
    }
    const size_t stop_index = 0;
    const size_t dir_index = poll_fds.size();
    if (dir_watch.has_value()) {
      poll_fds.push_back(PollSharedFd{
          .fd = dir_watch->ReadyFd(), .events = POLLIN, .revents = 0});
    } else {
      missing_source = true;
    }
    for (MonitorSource* source : sources) {
      if (source == nullptr) {
        continue;
      }
      SharedFD ready_fd = source->ReadyFd();
      if (!ready_fd->IsOpen()) {
        missing_source = true;
        continue;
      }
      poll_fds.push_back(
          PollSharedFd{.fd = ready_fd, .events = POLLIN, .revents = 0});
    }

    // Block until file changes occur. If any watch failed or is missing, use
    // a fallback timeout to awake and retry.
    int poll_res = SharedFD::Poll(poll_fds, missing_source ? 200 : -1);
    CF_EXPECT_GE(poll_res, 0, StrError());

    if (stop_fd->IsOpen() && (poll_fds[stop_index].revents & POLLIN)) {
      return {};
    }

    if (dir_watch.has_value() && (poll_fds[dir_index].revents & POLLIN)) {
      uint32_t events = dir_watch->Drain().value_or(FileWatch::kRemove);
      if ((events & FileWatch::kRemove) > 0) {
        launcher_monitor_source.reset();
        kernel_monitor_source.reset();
        logcat_monitor_source.reset();
      }
    }

    if (missing_source) {
      launcher_monitor_source.reset();  // will cut over to launcher.log soon
    }
  }
  return {};
}

}  // namespace cuttlefish
