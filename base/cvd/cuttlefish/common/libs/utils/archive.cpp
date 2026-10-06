/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "cuttlefish/common/libs/utils/archive.h"

#include <unistd.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"

#include "cuttlefish/process/command.h"
#include "cuttlefish/process/managed_stdio.h"
#include "cuttlefish/result/result.h"

namespace cuttlefish {
namespace {

// Debian installs libarchive's tar as /usr/bin/bsdtar; the BSD ports trees,
// pkgsrc and Homebrew put it under /usr/local, /usr/pkg or /opt, while on
// FreeBSD and macOS /usr/bin/tar is itself libarchive's tar.
const std::string& TarPath() {
  static const std::string path = [] {
    for (const char* candidate : {
             "/usr/bin/bsdtar",
             "/usr/local/bin/bsdtar",
             "/usr/pkg/bin/bsdtar",
             "/opt/homebrew/bin/bsdtar",
             "/opt/local/bin/bsdtar",
#if defined(__FreeBSD__) || defined(__APPLE__)
             "/usr/bin/tar",
#endif
         }) {
      if (access(candidate, X_OK) == 0) {
        return std::string(candidate);
      }
    }
    return std::string("bsdtar");
  }();
  return path;
}

Result<std::vector<std::string>> ExtractHelper(
    std::vector<std::string>& files, const std::string& archive_filepath,
    const std::string& target_directory, const bool keep_archive) {
  CF_EXPECT(!files.empty(), "No files extracted from " << archive_filepath);

  auto it = files.begin();
  while (it != files.end()) {
    if (it->empty() || absl::EndsWith(*it, "/")) {
      it = files.erase(it);
    } else {
      *it = target_directory + "/" + *it;
      it++;
    }
  }

  if (!keep_archive && unlink(archive_filepath.data()) != 0) {
    LOG(ERROR) << "Could not delete " << archive_filepath;
    files.push_back(archive_filepath);
  }

  return {files};
}

Result<std::vector<std::string>> ExtractFiles(
    const std::string& archive, const std::vector<std::string>& to_extract,
    const std::string& target_directory) {
  Command tar_cmd = Command(TarPath())
                        .AddParameter("-x")
                        .AddParameter("-v")
                        .AddParameter("-C")
                        .AddParameter(target_directory)
                        .AddParameter("-f")
                        .AddParameter(archive)
                        .AddParameter("-S");
  for (const auto& extract : to_extract) {
    tar_cmd.AddParameter(extract);
  }
  std::string tar_stdout;
  std::string tar_stderr;
  int exit_code = RunWithManagedStdio(std::move(tar_cmd), nullptr, &tar_stdout,
                                      &tar_stderr);
  CF_EXPECTF(exit_code == 0,
             "Failed to execute 'bsdtar' <args>: exit code = {}, stdout = "
             "'{}', stderr = '{}'",
             exit_code, tar_stdout, tar_stderr);
  VLOG(0) << tar_stderr;

  std::vector<std::string_view> split = absl::StrSplit(tar_stderr, '\n');
  std::vector<std::string> outputs;
  outputs.reserve(split.size());
  for (std::string_view& view : split) {
    absl::ConsumePrefix(&view, "x ");
    outputs.emplace_back(view);
  }

  return outputs;
}

Result<std::vector<std::string>> ExtractAll(
    const std::string& archive, const std::string& target_directory) {
  std::vector<std::string> out =
      CF_EXPECT(ExtractFiles(archive, {}, target_directory));
  return out;
}

}  // namespace

Result<std::vector<std::string>> ExtractImages(
    const std::string& archive_filepath, const std::string& target_directory,
    const std::vector<std::string>& images, const bool keep_archive) {
  CF_EXPECT(ExtractFiles(archive_filepath, images, target_directory),
            "Could not extract images from \"" << archive_filepath << "\" to \""
                                               << target_directory << "\"");

  std::vector<std::string> files = images;
  return ExtractHelper(files, archive_filepath, target_directory, keep_archive);
}

Result<std::string> ExtractImage(const std::string& archive_filepath,
                                 const std::string& target_directory,
                                 const std::string& image,
                                 const bool keep_archive) {
  std::vector<std::string> result = CF_EXPECT(
      ExtractImages(archive_filepath, target_directory, {image}, keep_archive));
  return {result.front()};
}

Result<std::vector<std::string>> ExtractArchiveContents(
    const std::string& archive_filepath, const std::string& target_directory,
    const bool keep_archive) {
  std::vector<std::string> files =
      CF_EXPECT(ExtractAll(archive_filepath, target_directory),
                "Could not extract \"" << archive_filepath << "\" to \""
                                       << target_directory << "\"");

  return ExtractHelper(files, archive_filepath, target_directory, keep_archive);
}

std::string ExtractArchiveToMemory(const std::string& archive_filepath,
                                   const std::string& archive_member) {
  Command tar_cmd(TarPath());
  tar_cmd.AddParameter("-xf");
  tar_cmd.AddParameter(archive_filepath);
  tar_cmd.AddParameter("-O");
  tar_cmd.AddParameter(archive_member);
  Result<std::string> stdout_str = RunAndCaptureStdout(std::move(tar_cmd));

  if (!stdout_str.has_value()) {
    LOG(ERROR) << "Could not extract \"" << archive_member << "\" from \""
               << archive_filepath << "\" to memory: " << stdout_str.error();
    return "";
  }
  return *stdout_str;
}

std::vector<std::string> ArchiveContents(const std::string& archive) {
  Command tar_cmd(TarPath());
  tar_cmd.AddParameter("-tf");
  tar_cmd.AddParameter(archive);

  Result<std::string> tar_output = RunAndCaptureStdout(std::move(tar_cmd));
  if (tar_output.has_value()) {
    return absl::StrSplit(*tar_output, '\n');
  } else {
    LOG(ERROR) << "`bsdtar -tf '" << archive
               << "'`failed: " << tar_output.error();
    return {};
  }
}

}  // namespace cuttlefish
