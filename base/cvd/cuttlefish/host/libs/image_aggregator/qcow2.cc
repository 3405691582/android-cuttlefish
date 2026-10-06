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

#include "cuttlefish/host/libs/image_aggregator/qcow2.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#ifndef __linux__
#include <unistd.h>
#endif

#include <memory>
#include <string>
#ifndef __linux__
#include <string_view>
#endif
#include <utility>

#ifndef __linux__
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#endif

#include "cuttlefish/common/libs/fs/fd.h"
#include "cuttlefish/common/libs/utils/cf_endian.h"
#include "cuttlefish/io/read_exact.h"
#include "cuttlefish/process/command.h"
#include "cuttlefish/process/managed_stdio.h"
#include "cuttlefish/result/result.h"

namespace cuttlefish {

namespace {

#ifndef __linux__
// `qemu-img` from $PATH, falling back to the usual package locations. Used to
// create overlays on hosts where crosvm is not available (e.g. the BSDs,
// where only the QEMU VM manager is supported).
Result<std::string> QemuImgPath() {
  const char* path_env = getenv("PATH");
  std::string search = path_env ? path_env : "";
  absl::StrAppend(&search, ":/usr/local/bin:/usr/bin:/opt/homebrew/bin");
  for (std::string_view dir : absl::StrSplit(search, ':', absl::SkipEmpty())) {
    std::string candidate = absl::StrCat(dir, "/qemu-img");
    if (access(candidate.c_str(), X_OK) == 0) {
      return candidate;
    }
  }
  return CF_ERR("qemu-img not found in PATH");
}
#endif

struct __attribute__((packed)) QcowHeader {
  Be32 magic;
  Be32 version;
  Be64 backing_file_offset;
  Be32 backing_file_size;
  Be32 cluster_bits;
  Be64 size;
  Be32 crypt_method;
  Be32 l1_size;
  Be64 l1_table_offset;
  Be64 refcount_table_offset;
  Be32 refcount_table_clusters;
  Be32 nb_snapshots;
  Be64 snapshots_offset;
};

static_assert(sizeof(QcowHeader) == 72);

}  // namespace

struct Qcow2Image::Impl {
  QcowHeader header_;
};

Result<Qcow2Image> Qcow2Image::Create(const std::string& crosvm_path,
                                      const std::string& backing_file,
                                      std::string output_overlay_path) {
#ifdef __linux__
  Command create_cmd = Command(crosvm_path)
                           .AddParameter("create_qcow2")
                           .AddParameter("--backing-file")
                           .AddParameter(backing_file)
                           .AddParameter(output_overlay_path);
#else
  // crosvm is Linux-only (a Linux prebuilt may still be present in a fetched
  // host package, but cannot run here); qemu-img produces an equivalent
  // overlay. The backing format must be given explicitly (qemu-img >= 6.1
  // refuses to probe it) and the composite disk is a raw image.
  (void)crosvm_path;
  Command create_cmd = Command(CF_EXPECT(QemuImgPath()))
                           .AddParameter("create")
                           .AddParameter("-q")
                           .AddParameter("-f")
                           .AddParameter("qcow2")
                           .AddParameter("-F")
                           .AddParameter("raw")
                           .AddParameter("-b")
                           .AddParameter(backing_file)
                           .AddParameter(output_overlay_path);
#endif

  CF_EXPECT(RunAndCaptureStdout(std::move(create_cmd)));

  return CF_EXPECT(OpenExisting(std::move(output_overlay_path)));
}

Result<Qcow2Image> Qcow2Image::OpenExisting(std::string path) {
  Fd fd = CF_EXPECT(Fd::Open(path, O_CLOEXEC, O_RDONLY));

  std::unique_ptr<Impl> impl(CF_EXPECT(new Impl()));

  impl->header_ = CF_EXPECT(ReadExactBinary<QcowHeader>(fd));

  std::string magic(reinterpret_cast<char*>(&impl->header_.magic),
                    MagicString().size());
  CF_EXPECT_EQ(magic, MagicString());

  return Qcow2Image(std::move(impl));
}

std::string Qcow2Image::MagicString() { return "QFI\xfb"; }

Qcow2Image::Qcow2Image(Qcow2Image&& other) { impl_ = std::move(other.impl_); }
Qcow2Image::~Qcow2Image() = default;
Qcow2Image& Qcow2Image::operator=(Qcow2Image&& other) {
  impl_ = std::move(other.impl_);
  return *this;
}

Result<uint64_t> Qcow2Image::VirtualSizeBytes() const {
  CF_EXPECT(impl_.get());

  return impl_->header_.size.as_uint64_t();
}

Qcow2Image::Qcow2Image(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

}  // namespace cuttlefish
