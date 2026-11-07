//
// Copyright (C) 2020 The Android Open Source Project
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

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>

#include <regex>
#include <string>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "gflags/gflags.h"

#include "cuttlefish/common/libs/fs/shared_fd.h"
#include "cuttlefish/common/libs/utils/tee_logging.h"
#include "cuttlefish/host/libs/config/cuttlefish_config.h"
#include "cuttlefish/posix/strerror.h"
#include "cuttlefish/result/result_type.h"

using cuttlefish::StrError;

DEFINE_string(process_name, "", "The process to credit log messages to");
DEFINE_int32(log_fd_in, -1, "The file descriptor to read logs from.");

// Crosvm formats logs starting with a local ISO 8601 timestamp and then a
// log level (based on external/crosvm/base/src/syslog.rs).
const std::regex kCrosvmLogPattern(
    "^\\["
    "\\d{4}" /* year */
    "-"
    "\\d{2}" /* month */
    "-"
    "\\d{2}" /* day */
    "T"
    "\\d{2}" /* hour */
    ":"
    "\\d{2}" /* minute*/
    ":"
    "\\d{2}" /* second */
    "\\."
    "\\d{9}"                          /* millisecond */
    "(Z|[+-]\\d{2}(:\\d{2}|\\d{2})?)" /* timezone */
    "\\s"
    "(ERROR|WARN|INFO|DEBUG|TRACE)");

// Write end of the self-pipe the SIGINT handler reports through.
int sigint_pipe_write = -1;

void OnSigint(int) {
  char byte = 0;
  // The pipe is non-blocking; a full pipe means a wakeup is pending.
  (void)!write(sigint_pipe_write, &byte, sizeof(byte));
}

int main(int argc, char** argv) {
  google::ParseCommandLineFlags(&argc, &argv, /* remove_flags */ true);

  CHECK(FLAGS_log_fd_in >= 0) << "-log_fd_in is required";

  auto config = cuttlefish::CuttlefishConfig::Get();

  CHECK(config) << "Could not open cuttlefish config";

  auto instance = config->ForDefaultInstance();

  if (instance.run_as_daemon()) {
    cuttlefish::LogToFiles({instance.launcher_log_path()});
  } else {
    cuttlefish::LogToStderrAndFiles({instance.launcher_log_path()});
  }

  auto log_fd = cuttlefish::SharedFD::Dup(FLAGS_log_fd_in);
  CHECK(log_fd->IsOpen()) << "Failed to dup log_fd_in: " << log_fd->StrError();
  close(FLAGS_log_fd_in);

  if (!FLAGS_process_name.empty()) {
    LOG(WARNING) << "Log tags unsupported with abseil logging: "
                 << FLAGS_process_name;
  }

  // Turn SIGINT into a readable fd (self-pipe) so the loop below can poll
  // for it; the signal may arrive blocked from the parent, so unblock it.
  int sigint_fds[2];
  CHECK_EQ(pipe(sigint_fds), 0) << "pipe failed: " << StrError();
  CHECK_NE(fcntl(sigint_fds[1], F_SETFL, O_NONBLOCK), -1)
      << "fcntl failed: " << StrError();
  sigint_pipe_write = sigint_fds[1];
  struct sigaction action = {};
  action.sa_handler = OnSigint;
  action.sa_flags = SA_RESTART;  // poll is still interrupted, see below
  sigemptyset(&action.sa_mask);
  CHECK_EQ(sigaction(SIGINT, &action, nullptr), 0)
      << "sigaction failed: " << StrError();
  sigset_t mask;  // NOLINT(misc-include-cleaner): <signal.h>
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  CHECK_EQ(sigprocmask(SIG_UNBLOCK, &mask, nullptr), 0)
      << "sigprocmask failed: " << StrError();
  auto int_fd = cuttlefish::SharedFD::Dup(sigint_fds[0]);
  CHECK(int_fd->IsOpen()) << "Failed to dup pipe: " << int_fd->StrError();
  close(sigint_fds[0]);

  auto poll_fds = std::vector<cuttlefish::PollSharedFd>{
      cuttlefish::PollSharedFd{
          .fd = log_fd,
          .events = POLLIN,  // NOLINT(misc-include-cleaner): poll.h
          .revents = 0,
      },
      cuttlefish::PollSharedFd{
          .fd = int_fd,
          .events = POLLIN,  // NOLINT(misc-include-cleaner): poll.h
          .revents = 0,
      },
  };

  VLOG(0) << "Starting to read from process " << FLAGS_process_name;

  char buf[1 << 16];
  cuttlefish::Result<uint64_t> chars_read = 0;
  for (;;) {
    // We can assume all writers to `log_fd` have completed before a SIGINT is
    // sent, but we need to make sure we've actually read all the data before
    // exiting. So, keep reading from `log_fd` until both (1) we get SIGINT and
    // (2) `log_fd` is empty (but not necessarily EOF).
    //
    // This could be simpler if all the writers would close their FDs when they
    // are finished. Then, we could just read until EOF. However that would
    // require more work elsewhere in cuttlefish.
    int polled = cuttlefish::SharedFD::Poll(poll_fds, /*timeout=*/-1);
    if (polled < 0 && errno == EINTR) {
      continue;
    }
    CHECK(polled >= 0) << "poll failed: " << StrError();
    if (poll_fds[0].revents) {
      chars_read = log_fd->Read(buf, sizeof(buf));
      if (!chars_read.has_value()) {
        VLOG(0) << "Failed to read from process " << FLAGS_process_name << ": "
                << log_fd->StrError();
        break;
      }
      if (*chars_read == 0) {
        break;
      }
      std::string trimmed(
          absl::StripAsciiWhitespace(std::string_view(buf, *chars_read)));
      // Newlines inside `trimmed` are handled by the android logging code.
      // These checks attempt to determine the log severity coming from crosvm.
      // There is no guarantee of success all the time since log line boundaries
      // could be out sync with the reads, but that's ok.
      if (absl::StartsWith(trimmed, "[INFO")) {
        VLOG(0) << trimmed;
      } else if (absl::StartsWith(trimmed, "[ERROR")) {
        LOG(ERROR) << trimmed;
      } else if (absl::StartsWith(trimmed, "[WARNING")) {
        LOG(WARNING) << trimmed;
      } else if (absl::StartsWith(trimmed, "[VERBOSE")) {
        VLOG(1) << trimmed;
      } else {
        std::smatch match_result;
        if (std::regex_search(trimmed, match_result, kCrosvmLogPattern)) {
          if (match_result.size() == 4) {
            const auto& level = match_result[3];
            if (level == "ERROR") {
              LOG(ERROR) << trimmed;
            } else if (level == "WARN") {
              LOG(WARNING) << trimmed;
            } else if (level == "INFO") {
              if (trimmed.find("disk] Disk image file is hosted") !=
                      std::string::npos ||
                  trimmed.find("disk] disk size") != std::string::npos) {
                VLOG(0) << trimmed;
              } else {
                LOG(INFO) << trimmed;
              }
            } else if (level == "DEBUG") {
              VLOG(0) << trimmed;
            } else if (level == "TRACE") {
              VLOG(1) << trimmed;
            } else {
              VLOG(0) << trimmed;
            }
          }
        } else {
          VLOG(0) << trimmed;
        }
      }

      // Go back to polling immediately to see if there is more data, don't
      // handle any signals yet.
      continue;
    }
    if (poll_fds[1].revents) {
      char byte;
      CHECK_EQ(int_fd->Read(&byte, sizeof(byte)).value_or(0), 1)
          << "bad read on the SIGINT pipe: " << int_fd->StrError();
      break;
    }
  }

  VLOG(0) << "Finished reading from process " << FLAGS_process_name;
}
