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
// everything below it. Symlinks are not followed (their own size counts, as
// with `du --apparent-size`) and hard-linked files are only counted once.
// Implemented in-process rather than via `du -s --apparent-size
// --block-size=...`, which only GNU coreutils' du understands.
Result<size_t> ApparentSize(const std::string& path) {
  std::set<std::pair<dev_t, ino_t>> seen;
  size_t total = 0;

  auto add = [&seen, &total](const std::string& p) -> Result<void> {
    struct stat st;
    CF_EXPECTF(lstat(p.c_str(), &st) == 0, "lstat(\"{}\") failed: {}", p,
               StrError(errno));
    if (st.st_nlink > 1 && !seen.emplace(st.st_dev, st.st_ino).second) {
      return {};
    }
    total += static_cast<size_t>(st.st_size);
    return {};
  };

  CF_EXPECT(add(path));

  std::error_code ec;
  if (!std::filesystem::is_directory(
          std::filesystem::symlink_status(path, ec))) {
    return total;
  }
  CF_EXPECTF(!ec, "symlink_status(\"{}\") failed: {}", path, ec.message());

  std::filesystem::recursive_directory_iterator it(
      path, std::filesystem::directory_options::skip_permission_denied, ec);
  CF_EXPECTF(!ec, "Unable to iterate \"{}\": {}", path, ec.message());
  const std::filesystem::recursive_directory_iterator end;
  while (it != end) {
    CF_EXPECT(add(it->path().native()));
    it.increment(ec);
    CF_EXPECTF(!ec, "Unable to iterate \"{}\": {}", path, ec.message());
  }
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
  // Round up like `du --block-size=1G` does.
  static constexpr size_t kGigabyte = size_t{1} << 30;
  return (bytes + kGigabyte - 1) / kGigabyte;
}

}  // namespace cuttlefish
