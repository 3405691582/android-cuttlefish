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

#include "cuttlefish/common/libs/utils/disk_usage.h"

#include <errno.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <utility>

#include "cuttlefish/posix/strerror.h"
#include "cuttlefish/result/result.h"

namespace cuttlefish {
namespace {

// Apparent size (sum of st_size) of `path` and, for a directory, of
// everything below it, as `du -s --apparent-size` reports it: symlinks below
// `path` are not followed and hard-linked files are counted once.
Result<size_t> ApparentSize(const std::string& path) {
  struct stat st;
  CF_EXPECTF(stat(path.c_str(), &st) == 0, "stat(\"{}\") failed: {}", path,
             StrError(errno));
  if (S_ISREG(st.st_mode)) {
    return static_cast<size_t>(st.st_size);
  }

  CF_EXPECTF(S_ISDIR(st.st_mode),
             "\"{}\" is neither a regular file nor a directory", path);
  size_t total = static_cast<size_t>(st.st_size);
  // Hard links show up once per name; count each inode once.
  std::set<std::pair<dev_t, ino_t>> counted_inodes;
  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(
      path, std::filesystem::directory_options::skip_permission_denied, ec);
  CF_EXPECTF(!ec, "Unable to iterate \"{}\": {}", path, ec.message());
  const std::filesystem::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    const std::string& entry = it->path().native();
    CF_EXPECTF(lstat(entry.c_str(), &st) == 0, "lstat(\"{}\") failed: {}",
               entry, StrError(errno));
    if (S_ISREG(st.st_mode) && st.st_nlink > 1) {
      const std::pair<dev_t, ino_t> inode(st.st_dev, st.st_ino);
      if (counted_inodes.contains(inode)) {
        continue;
      }
      counted_inodes.insert(inode);
    }
    total += static_cast<size_t>(st.st_size);
  }
  CF_EXPECTF(!ec, "Unable to iterate \"{}\": {}", path, ec.message());
  return total;
}

}  // namespace

Result<size_t> GetDiskUsageBytes(const std::string& path) {
  return CF_EXPECTF(ApparentSize(path),
                    "Unable to determine disk usage of file \"{}\"", path);
}

Result<size_t> GetDiskUsageGigabytes(const std::string& path) {
  size_t bytes =
      CF_EXPECTF(ApparentSize(path),
                 "Unable to determine disk usage of file \"{}\"", path);
  static constexpr size_t kGigabyte = size_t{1} << 30;
  return (bytes + kGigabyte - 1) / kGigabyte;
}

}  // namespace cuttlefish
