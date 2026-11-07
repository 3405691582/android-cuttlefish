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

#include "cuttlefish/host/commands/cvd/cli/commands/monitor/file_monitor_source.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "android-base/file.h"

#include "cuttlefish/common/libs/fs/shared_fd.h"
#include "cuttlefish/common/libs/utils/file_watch.h"
#include "cuttlefish/host/commands/cvd/cli/commands/monitor/monitor_source.h"
#include "cuttlefish/host/commands/cvd/cli/format_byte_size.h"
#include "cuttlefish/io/read_exact.h"
#include "cuttlefish/io/reader_seeker.h"
#include "cuttlefish/result/expect.h"
#include "cuttlefish/result/result_type.h"

namespace cuttlefish {
namespace {

constexpr size_t kChunkReadSize = 1 << 20;
constexpr size_t kMaximumScrollback = 1 << 24;

struct FileData {
  std::vector<std::string> last_lines;
  size_t total_size = 0;
};

Result<FileData> GetLastNLines(
    ReaderSeeker& rs, size_t n,
    std::function<Result<bool>(std::string_view)>& filter_line,
    std::string name) {
  FileData ret;
  ret.total_size = CF_EXPECT(rs.SeekEnd(0));

  std::string accumulated_data;
  size_t offset = ret.total_size;

  // TODO(schuffelen): make FileMonitorSource stateful to avoid re-reading
  while (offset > 0 && ret.total_size - offset < kMaximumScrollback) {
    size_t to_read = std::min(kChunkReadSize, offset);
    offset -= to_read;
    CF_EXPECT(rs.SeekSet(offset));

    std::string chunk(to_read, '\0');
    CF_EXPECT(ReadExact(rs, chunk.data(), to_read));

    accumulated_data = chunk + accumulated_data;
    std::vector<std::string_view> lines =
        absl::StrSplit(accumulated_data, '\n');
    if (lines.empty()) {
      continue;
    }
    auto it = lines.begin();
    it++;  // ignore the first line, it may be partially read
    auto predicate = [&filter_line](std::string_view line) {
      return line.empty() || !filter_line(line).value_or(false);
    };
    auto end = std::remove_if(it, lines.end(), predicate);
    const size_t remaining = end - it;
    if (remaining > n) {
      it += remaining - n;
    }
    ret.last_lines.clear();
    ret.last_lines.reserve(lines.size());
    for (; it != end; it++) {
      ret.last_lines.emplace_back(*it);
    }
    if (ret.last_lines.size() >= n) {
      break;
    }
  }

  return ret;
}

}  // namespace

FileMonitorSource::FileMonitorSource(
    std::string path, std::unique_ptr<ReaderSeeker> file_io,
    std::function<Result<std::string>(std::string_view)> colorize_line,
    std::function<Result<bool>(std::string_view)> filter_line)
    : path_(std::move(path)),
      file_io_(std::move(file_io)),
      colorize_line_(std::move(colorize_line)),
      filter_line_(std::move(filter_line)) {
  Result<FileWatch> watch =
      FileWatch::Create(path_, FileWatch::kModify | FileWatch::kDelete);
  CHECK(watch.has_value()) << watch.error().FormatForEnv(true);
  watch_ = std::make_unique<FileWatch>(std::move(*watch));
}

FileMonitorSource::~FileMonitorSource() = default;

MonitorOutput FileMonitorSource::Report(size_t rows, size_t) {
  const std::string basename = android::base::Basename(path_);
  Result<FileData> file_data =
      GetLastNLines(*file_io_, rows, filter_line_, basename);
  if (!file_data.has_value()) {
    return MonitorOutput(
        absl::StrCat(basename, " (error)"),
        absl::StrSplit(file_data.error().FormatForEnv(true), '\n'));
  }
  for (std::string& line : file_data->last_lines) {
    line = colorize_line_(line).value_or(line);
  }
  std::string size = FormatByteSize(file_data->total_size);
  Result<uint32_t> unused = watch_->Drain();
  return MonitorOutput(absl::StrCat(basename, " (", size, ")"),
                       file_data->last_lines);
}

SharedFD FileMonitorSource::ReadyFd() { return watch_->ReadyFd(); }

}  // namespace cuttlefish
