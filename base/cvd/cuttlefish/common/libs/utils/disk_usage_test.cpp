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

#include "cuttlefish/common/libs/utils/disk_usage.h"

#include <stddef.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <ios>
#include <string>

#include "android-base/file.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "cuttlefish/result/result_matchers.h"

namespace cuttlefish {
namespace {

using ::testing::Ge;

void WriteFile(const std::string& path, size_t size) {
  std::ofstream out(path, std::ios::binary);
  ASSERT_TRUE(out.good()) << path;
  out << std::string(size, 'x');
}

size_t LstatSize(const std::string& path) {
  struct stat st;
  EXPECT_EQ(lstat(path.c_str(), &st), 0) << path;
  return static_cast<size_t>(st.st_size);
}

TEST(DiskUsageTest, RegularFile) {
  TemporaryDir dir;
  const std::string file = dir.path + std::string("/file");
  WriteFile(file, 12345);

  EXPECT_THAT(GetDiskUsageBytes(file), IsOkAndValue(size_t{12345}));
  EXPECT_THAT(GetDiskUsageGigabytes(file), IsOkAndValue(size_t{1}));
}

TEST(DiskUsageTest, EmptyFile) {
  TemporaryDir dir;
  const std::string file = dir.path + std::string("/empty");
  WriteFile(file, 0);

  EXPECT_THAT(GetDiskUsageBytes(file), IsOkAndValue(size_t{0}));
  EXPECT_THAT(GetDiskUsageGigabytes(file), IsOkAndValue(size_t{0}));
}

TEST(DiskUsageTest, MissingFileIsAnError) {
  TemporaryDir dir;
  EXPECT_THAT(GetDiskUsageBytes(dir.path + std::string("/missing")), IsError());
}

TEST(DiskUsageTest, DirectoryIsApparentSizeOfContents) {
  TemporaryDir dir;
  const std::string root = dir.path;
  ASSERT_EQ(mkdir((root + "/sub").c_str(), 0755), 0);
  WriteFile(root + "/a", 1000);
  WriteFile(root + "/sub/b", 2048);
  // A hard link must not be counted twice.
  ASSERT_EQ(link((root + "/a").c_str(), (root + "/sub/a_link").c_str()), 0);
  // A symlink contributes its own (tiny) size, not its target's.
  ASSERT_EQ(symlink("a", (root + "/sub/a_sym").c_str()), 0);

  // Directory inodes themselves count too (as with `du --apparent-size`),
  // and their st_size is filesystem dependent.
  const size_t expected = 1000 + 2048 + LstatSize(root) +
                          LstatSize(root + "/sub") +
                          LstatSize(root + "/sub/a_sym");

  EXPECT_THAT(GetDiskUsageBytes(root), IsOkAndValue(expected));
  EXPECT_THAT(GetDiskUsageGigabytes(root), IsOkAndValue(size_t{1}));
  EXPECT_THAT(GetDiskUsageBytes(root + "/sub"),
              IsOkAndValue(Ge(size_t{2048 + 1000})));
}

}  // namespace
}  // namespace cuttlefish
