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
#pragma once

#include <stdint.h>

#include <string>

#include "cuttlefish/common/libs/fs/shared_fd.h"
#include "cuttlefish/result/result.h"

namespace cuttlefish {

/*
 * A pollable source of file system events on one path: a directory for
 * entries appearing in it, or a file for changes to it. ReadyFd() is
 * readable, per poll(2), whenever Drain() has something to report.
 *
 * Linux backs this with inotify; the BSDs and macOS with a kqueue
 * EVFILT_VNODE filter. kqueue cannot name the entry that changed and
 * cannot tell an entry's creation from its removal, so callers re-check
 * the path they care about after a wake-up rather than relying on the
 * mask alone.
 */
class FileWatch {
 public:
  enum Event : uint32_t {
    kCreate = 1 << 0,  // an entry appeared in the watched directory
    kModify = 1 << 1,  // the watched file was written or extended
    kDelete = 1 << 2,  // the watched file was unlinked or renamed away
    kRemove = 1 << 3,  // an entry disappeared from the watched directory
  };

  static Result<FileWatch> Create(const std::string& path, uint32_t events);

  SharedFD ReadyFd() const { return ready_fd_; }

  // Non-blocking. The union of the events seen since the last call,
  // restricted to the ones asked for.
  Result<uint32_t> Drain();

 private:
  FileWatch(SharedFD ready_fd, SharedFD watched, uint32_t events)
      : ready_fd_(std::move(ready_fd)),
        watched_(std::move(watched)),
        events_(events) {}

  SharedFD ready_fd_;
  SharedFD watched_;  // the kqueue driver's open path; unused on Linux
  uint32_t events_;
};

}  // namespace cuttlefish
