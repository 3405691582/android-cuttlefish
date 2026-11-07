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

#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <utility>

#include "gtest/gtest.h"

#include "cuttlefish/common/libs/fs/shared_fd.h"
#include "cuttlefish/process/command.h"
#include "cuttlefish/process/subprocess.h"
#include "cuttlefish/process/subprocess_options.h"
#include "cuttlefish/result/result.h"

namespace cuttlefish {
namespace {

/*
 * The test forks an intermediate that spawns a shell through Command,
 * then ends the intermediate. The shell reports SIGHUP on a pipe the
 * test holds, so the outcome is observable without being able to wait
 * for a reparented grandchild.
 */
class ParentDeathHelperTest : public ::testing::Test {
 protected:
  void SetUp() override {
    setenv("CUTTLEFISH_PARENT_DEATH_HELPER", "1", 1);
    ASSERT_TRUE(SharedFD::Pipe(&read_end_, &write_end_));
  }

  // Forks the intermediate; `body` runs in it with the shell's output
  // wired to the test's pipe and must not return.
  template <typename F>
  pid_t Intermediate(F body) {
    pid_t pid = fork();
    if (pid == 0) {
      read_end_->Close();
      body();
      _exit(0);
    }
    write_end_->Close();
    return pid;
  }

  Subprocess SpawnReporter(SubprocessOptions options) {
    Command cmd("sh");
    cmd.AddParameter("-c");
    cmd.AddParameter("trap 'echo HUP; exit 0' HUP; while :; do sleep 1; done");
    cmd.RedirectStdIO(Command::StdIoChannel::kStdOut, write_end_);
    return cmd.Start(std::move(options));
  }

  // What the shell wrote before the pipe closed or `timeout_ms` passed.
  std::string ReadReport(int timeout_ms) {
    std::string report;
    // NOLINTNEXTLINE(misc-include-cleaner): <poll.h>
    PollSharedFd pfd = {read_end_, POLLIN, 0};
    while (SharedFD::Poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLIN)) {
      char buf[64];
      Result<uint64_t> n = read_end_->Read(buf, sizeof(buf));
      if (!n.has_value() || *n == 0) {
        break;
      }
      report.append(buf, *n);
    }
    return report;
  }

  SharedFD read_end_;
  SharedFD write_end_;
};

TEST_F(ParentDeathHelperTest, KilledSpawnerHangsUpChild) {
  pid_t intermediate = Intermediate([this] {
    SpawnReporter(SubprocessOptions().Verbose(false));
    pause();
  });
  // Give the child time to exec and install its trap.
  usleep(300 * 1000);
  ASSERT_EQ(kill(intermediate, SIGKILL), 0);
  ASSERT_EQ(waitpid(intermediate, nullptr, 0), intermediate);
  EXPECT_EQ(ReadReport(5000), "HUP\n");
}

TEST_F(ParentDeathHelperTest, ExitedSpawnerHangsUpChildAfterReaping) {
  Intermediate([this] {
    // A reaped child is forgotten; the one still running is signalled.
    Command("true").Start(SubprocessOptions().Verbose(false)).Wait();
    SpawnReporter(SubprocessOptions().Verbose(false));
    usleep(300 * 1000);
  });
  EXPECT_EQ(ReadReport(5000), "HUP\n");
}

TEST_F(ParentDeathHelperTest, UnregisteredChildOutlivesSpawner) {
  pid_t intermediate = Intermediate([this] {
    SpawnReporter(SubprocessOptions().Verbose(false).ExitWithParent(false));
    pause();
  });
  usleep(300 * 1000);
  ASSERT_EQ(kill(intermediate, SIGKILL), 0);
  ASSERT_EQ(waitpid(intermediate, nullptr, 0), intermediate);
  EXPECT_EQ(ReadReport(1500), "");
  // Clean up the deliberately orphaned shell and its sleep.
  system("pkill -HUP -f 'echo HUP; exit 0' >/dev/null 2>&1");
}

}  // namespace
}  // namespace cuttlefish
