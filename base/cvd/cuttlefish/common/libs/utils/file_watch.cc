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

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#ifdef __linux__
#include <sys/inotify.h>
#else
#include <sys/event.h>
#endif

#include <string>
#include <utility>

#include "cuttlefish/common/libs/fs/fd.h"
#include "cuttlefish/common/libs/fs/shared_fd.h"
#include "cuttlefish/posix/strerror.h"
#include "cuttlefish/result/result.h"

namespace cuttlefish {

#ifdef __linux__

Result<FileWatch> FileWatch::Create(const std::string& path, uint32_t events) {
  SharedFD inotify = CF_EXPECT(Fd::InotifyFd());
  uint32_t mask = 0;
  if (events & kCreate) {
    mask |= IN_CREATE;
  }
  if (events & kModify) {
    mask |= IN_MODIFY;
  }
  if (events & kDelete) {
    mask |= IN_DELETE_SELF | IN_MOVE_SELF;
  }
  if (events & kRemove) {
    mask |= IN_DELETE;
  }
  CF_EXPECTF(inotify->InotifyAddWatch(path, mask) >= 0,
             "Failed to watch '{}': {}", path, StrError());
  const int flags = inotify->Fcntl(F_GETFL, 0);
  CF_EXPECTF(inotify->Fcntl(F_SETFL, flags | O_NONBLOCK) >= 0,
             "Failed to make the inotify descriptor non-blocking: {}",
             inotify->StrError());
  return FileWatch(std::move(inotify), SharedFD(), events);
}

Result<uint32_t> FileWatch::Drain() {
  uint32_t seen = 0;
  char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
  Result<uint64_t> read_res = 0;
  while ((read_res = ready_fd_->Read(buf, sizeof(buf))).value_or(0) > 0) {
    for (char* ptr = buf; ptr < buf + *read_res;) {
      inotify_event* event = reinterpret_cast<inotify_event*>(ptr);
      if (event->mask & IN_CREATE) {
        seen |= kCreate;
      }
      if (event->mask & IN_MODIFY) {
        seen |= kModify;
      }
      if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
        seen |= kDelete;
      }
      if (event->mask & IN_DELETE) {
        seen |= kRemove;
      }
      ptr += sizeof(inotify_event) + event->len;
    }
  }
  CF_EXPECT(!read_res.has_value(),
            "Unexpected End-of-File reading inotify descriptor");
  const int err = ready_fd_->GetErrno();
  CF_EXPECTF(err == EAGAIN || err == EWOULDBLOCK,
             "Unexpected error reading inotify descriptor: {}", StrError(err));
  return seen & events_;
}

#else

Result<FileWatch> FileWatch::Create(const std::string& path, uint32_t events) {
  int open_flags = O_RDONLY | O_CLOEXEC;
#ifdef O_EVTONLY
  // macOS: hold the path for notifications only, so that the watch does
  // not keep a volume from unmounting.
  open_flags = O_EVTONLY | O_CLOEXEC;
#endif
  if (events & (kCreate | kRemove)) {
    open_flags |= O_DIRECTORY;
  }
  SharedFD watched = CF_EXPECT(Fd::Open(path, open_flags));
  SharedFD kq = CF_EXPECT(Fd::Kqueue());
  uint32_t fflags = 0;
  // A new or removed entry is a write to the directory.
  if (events & (kCreate | kModify | kRemove)) {
    fflags |= NOTE_WRITE | NOTE_EXTEND;
  }
  if (events & kDelete) {
    fflags |= NOTE_DELETE | NOTE_RENAME;
  }
  CF_EXPECTF(kq->KqueueWatchVnode(*watched, fflags), "Failed to watch '{}'",
             path);
  return FileWatch(std::move(kq), std::move(watched), events);
}

Result<uint32_t> FileWatch::Drain() {
  uint32_t seen = 0;
  const struct timespec no_wait = {0, 0};
  struct kevent out[16];
  while (true) {
    const int n = ready_fd_->Kevent(nullptr, 0, out, 16, &no_wait);
    CF_EXPECTF(n >= 0, "Unexpected error reading the kqueue: {}",
               ready_fd_->StrError());
    for (int i = 0; i < n; i++) {
      if (out[i].fflags & (NOTE_WRITE | NOTE_EXTEND)) {
        seen |= kCreate | kModify | kRemove;
      }
      if (out[i].fflags & (NOTE_DELETE | NOTE_RENAME)) {
        seen |= kDelete;
      }
    }
    if (n < 16) {
      break;
    }
  }
  return seen & events_;
}

#endif

}  // namespace cuttlefish
