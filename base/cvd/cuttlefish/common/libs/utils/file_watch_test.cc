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

#include "cuttlefish/common/libs/utils/file_watch.h"

#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "android-base/file.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "cuttlefish/common/libs/fs/shared_fd.h"
#include "cuttlefish/result/result.h"
#include "cuttlefish/result/result_matchers.h"

namespace cuttlefish {
namespace {

int PollWatch(const FileWatch& watch, int timeout_ms) {
  std::vector<PollSharedFd> fds = {
      // NOLINTNEXTLINE(misc-include-cleaner): <poll.h>
      PollSharedFd{.fd = watch.ReadyFd(), .events = POLLIN, .revents = 0},
  };
  return SharedFD::Poll(fds, timeout_ms);
}

void WriteFile(const std::string& path, const std::string& contents) {
  ASSERT_TRUE(android::base::WriteStringToFile(contents, path));
}

TEST(FileWatchTest, EntryAppearsInDirectory) {
  TemporaryDir dir;
  Result<FileWatch> watch = FileWatch::Create(dir.path, FileWatch::kCreate);
  ASSERT_THAT(watch, IsOk());
  EXPECT_EQ(PollWatch(*watch, 0), 0);

  WriteFile(std::string(dir.path) + "/new", "x");

  EXPECT_EQ(PollWatch(*watch, 5000), 1);
  EXPECT_THAT(watch->Drain(), IsOkAndValue(FileWatch::kCreate));
  EXPECT_EQ(PollWatch(*watch, 0), 0);
}

TEST(FileWatchTest, EntryRemovedFromDirectory) {
  TemporaryDir dir;
  const std::string path = std::string(dir.path) + "/file";
  WriteFile(path, "a");
  Result<FileWatch> watch = FileWatch::Create(dir.path, FileWatch::kRemove);
  ASSERT_THAT(watch, IsOk());

  ASSERT_EQ(unlink(path.c_str()), 0);

  EXPECT_EQ(PollWatch(*watch, 5000), 1);
  EXPECT_THAT(watch->Drain(), IsOkAndValue(FileWatch::kRemove));
}

TEST(FileWatchTest, FileModified) {
  TemporaryDir dir;
  const std::string path = std::string(dir.path) + "/file";
  WriteFile(path, "a");
  Result<FileWatch> watch =
      FileWatch::Create(path, FileWatch::kModify | FileWatch::kDelete);
  ASSERT_THAT(watch, IsOk());

  WriteFile(path, "ab");

  EXPECT_EQ(PollWatch(*watch, 5000), 1);
  EXPECT_THAT(watch->Drain(), IsOkAndValue(FileWatch::kModify));
}

TEST(FileWatchTest, FileDeleted) {
  TemporaryDir dir;
  const std::string path = std::string(dir.path) + "/file";
  WriteFile(path, "a");
  Result<FileWatch> watch = FileWatch::Create(path, FileWatch::kDelete);
  ASSERT_THAT(watch, IsOk());

  ASSERT_EQ(unlink(path.c_str()), 0);

  EXPECT_EQ(PollWatch(*watch, 5000), 1);
  Result<uint32_t> events = watch->Drain();
  ASSERT_THAT(events, IsOk());
  EXPECT_TRUE(*events & FileWatch::kDelete);
}

TEST(FileWatchTest, FileRenamedAway) {
  TemporaryDir dir;
  const std::string path = std::string(dir.path) + "/file";
  WriteFile(path, "a");
  Result<FileWatch> watch = FileWatch::Create(path, FileWatch::kDelete);
  ASSERT_THAT(watch, IsOk());

  ASSERT_EQ(rename(path.c_str(), (path + ".old").c_str()), 0);

  EXPECT_EQ(PollWatch(*watch, 5000), 1);
  Result<uint32_t> events = watch->Drain();
  ASSERT_THAT(events, IsOk());
  EXPECT_TRUE(*events & FileWatch::kDelete);
}

TEST(FileWatchTest, UnrequestedEventsAreMasked) {
  TemporaryDir dir;
  const std::string path = std::string(dir.path) + "/file";
  WriteFile(path, "a");
  Result<FileWatch> watch = FileWatch::Create(path, FileWatch::kDelete);
  ASSERT_THAT(watch, IsOk());

  WriteFile(path, "ab");

  if (PollWatch(*watch, 200) == 1) {
    EXPECT_THAT(watch->Drain(), IsOkAndValue(uint32_t{0}));
  }
}

TEST(FileWatchTest, Timeout) {
  TemporaryDir dir;
  Result<FileWatch> watch = FileWatch::Create(dir.path, FileWatch::kCreate);
  ASSERT_THAT(watch, IsOk());
  EXPECT_EQ(PollWatch(*watch, 100), 0);
  EXPECT_THAT(watch->Drain(), IsOkAndValue(uint32_t{0}));
}

TEST(FileWatchTest, MissingPath) {
  TemporaryDir dir;
  EXPECT_THAT(
      FileWatch::Create(std::string(dir.path) + "/nope", FileWatch::kCreate),
      IsError());
}

}  // namespace
}  // namespace cuttlefish
