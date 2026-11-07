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

#include "cuttlefish/process/parent_death_helper.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __linux__
#include <poll.h>
#include <sys/syscall.h>
#else
#include <sys/event.h>
#include <sys/time.h>
#endif

#include <mutex>

#include "cuttlefish/posix/strerror.h"
#include "cuttlefish/result/result.h"

namespace cuttlefish {
namespace {

struct Registration {
  pid_t pid;
  int32_t sig;  // 0 forgets the pid
};

constexpr int kMaxEntries = 1024;
// How long the helper waits for the pipe to reach EOF once the spawner
// is dead before it signals anyway (a non-exec'd child that never
// closes its inherited write end would otherwise hold it up forever).
constexpr int kDrainTimeoutMs = 5000;

// The write end of the helper's pipe and the pid of the process that
// forked the helper. A child forked without exec inherits both and
// must not mistake them for its own.
int helper_fd = -1;
pid_t helper_owner = -1;
std::mutex ensure_mutex;

/*
 * Everything below runs in the helper: a fork of a multithreaded
 * process that never execs, so only async-signal-safe calls and no
 * allocation.
 */
struct Entry {
  pid_t pid;
  int sig;
  int fd;  // the entry's pidfd on Linux; unused with kqueue
};

struct Helper {
  int pipe_fd;
  int wait_fd;  // the kqueue, or the spawner's pidfd on Linux
  pid_t parent;
  bool parent_alive;
  int count;
  Entry entries[kMaxEntries];
  Registration partial;
  size_t partial_bytes;
};

void Remove(Helper& h, pid_t pid) {
  for (int i = 0; i < h.count; i++) {
    if (h.entries[i].pid != pid) {
      continue;
    }
    if (h.entries[i].fd >= 0) {
      close(h.entries[i].fd);
    }
    h.entries[i--] = h.entries[--h.count];
  }
}

#ifdef __linux__

int PidFd(pid_t pid) {
#ifndef SYS_pidfd_open
  constexpr int SYS_pidfd_open = 434;
#endif
  return syscall(SYS_pidfd_open, pid, 0);
}

bool WatchParent(Helper& h) {
  h.wait_fd = PidFd(h.parent);
  return h.wait_fd >= 0;
}

bool WatchChild(Helper& h, Entry& e) {
  (void)h;
  e.fd = PidFd(e.pid);
  return e.fd >= 0;
}

/*
 * Blocks until the pipe is readable (returns 0), a watched process
 * exits (applied in place, returns 1) or the timeout passes (-1).
 */
int WaitOnce(Helper& h, int timeout_ms) {
  // NOLINTBEGIN(misc-include-cleaner): <poll.h>
  struct pollfd fds[kMaxEntries + 2];
  fds[0] = {h.pipe_fd, POLLIN, 0};
  fds[1] = {h.wait_fd, POLLIN, 0};  // -1 once the parent is gone
  for (int i = 0; i < h.count; i++) {
    fds[i + 2] = {h.entries[i].fd, POLLIN, 0};
  }
  int n;
  do {
    n = poll(fds, h.count + 2, timeout_ms);
  } while (n < 0 && errno == EINTR);
  // NOLINTEND(misc-include-cleaner)
  if (n <= 0) {
    return -1;
  }
  if (fds[1].revents) {
    h.parent_alive = false;
    close(h.wait_fd);
    h.wait_fd = -1;
  }
  for (int i = h.count - 1; i >= 0; i--) {
    if (fds[i + 2].revents) {
      Remove(h, h.entries[i].pid);
    }
  }
  return fds[0].revents ? 0 : 1;
}

#else

bool WatchProc(int kq, pid_t pid) {
  struct kevent change;
  EV_SET(&change, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
  return kevent(kq, &change, 1, nullptr, 0, nullptr) >= 0;
}

bool WatchParent(Helper& h) {
  h.wait_fd = kqueue();
  if (h.wait_fd < 0) {
    return false;
  }
  struct kevent change;
  EV_SET(&change, h.pipe_fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
  if (kevent(h.wait_fd, &change, 1, nullptr, 0, nullptr) < 0) {
    return false;
  }
  return WatchProc(h.wait_fd, h.parent);
}

bool WatchChild(Helper& h, Entry& e) {
  e.fd = -1;
  return WatchProc(h.wait_fd, e.pid);
}

int WaitOnce(Helper& h, int timeout_ms) {
  struct kevent out[16];
  struct timespec ts = {timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
  int n;
  do {
    n = kevent(h.wait_fd, nullptr, 0, out, 16, timeout_ms < 0 ? nullptr : &ts);
  } while (n < 0 && errno == EINTR);
  if (n <= 0) {
    return -1;
  }
  int rval = 1;
  for (int i = 0; i < n; i++) {
    if (out[i].filter == EVFILT_READ) {
      rval = 0;
    } else if (static_cast<pid_t>(out[i].ident) == h.parent) {
      h.parent_alive = false;
    } else {
      Remove(h, out[i].ident);
    }
  }
  return rval;
}

#endif

void Apply(Helper& h, const Registration& r) {
  if (r.sig == 0) {
    Remove(h, r.pid);
    return;
  }
  if (h.count == kMaxEntries) {
    return;
  }
  Entry& e = h.entries[h.count];
  e.pid = r.pid;
  e.sig = r.sig;
  // Failure means the child is already gone (ESRCH): nothing to signal.
  if (WatchChild(h, e)) {
    h.count++;
  }
}

// Applies what the pipe has; false at EOF.
bool ReadRegistrations(Helper& h) {
  Registration buf[64];
  char* base = reinterpret_cast<char*>(buf);
  // A write below PIPE_BUF is atomic but a read may still split it.
  memcpy(base, &h.partial, h.partial_bytes);
  ssize_t n;
  do {
    // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection): see Ensure
    n = read(h.pipe_fd, base + h.partial_bytes, sizeof(buf) - h.partial_bytes);
  } while (n < 0 && errno == EINTR);
  if (n <= 0) {
    return false;
  }
  const size_t have = h.partial_bytes + n;
  const size_t whole = have / sizeof(Registration);
  for (size_t i = 0; i < whole; i++) {
    Apply(h, buf[i]);
  }
  h.partial_bytes = have - whole * sizeof(Registration);
  memcpy(&h.partial, base + whole * sizeof(Registration), h.partial_bytes);
  return true;
}

/*
 * Inherited descriptors must not outlive the spawner in the helper:
 * a pipe the spawner's own parent reads to EOF would never close.
 */
void DropInheritedFds(int keep) {
  const int null = open("/dev/null", O_RDWR);
  if (null >= 0) {
    dup2(null, 0);
    dup2(null, 1);
    dup2(null, 2);
    close(null);
  }
  dup2(keep, 3);
  long max = sysconf(_SC_OPEN_MAX);
  if (max < 0 || max > 65536) {
    max = 65536;
  }
  for (int fd = 4; fd < max; fd++) {
    close(fd);
  }
}

[[noreturn]] void HelperMain(int pipe_fd, pid_t parent) {
  // The helper shares the spawner's process group; a signal aimed at
  // the group must not take it out before it has acted.
  sigset_t block;  // NOLINT(misc-include-cleaner): <signal.h>
  sigemptyset(&block);
  sigaddset(&block, SIGHUP);
  sigaddset(&block, SIGINT);
  sigaddset(&block, SIGTERM);
  sigprocmask(SIG_BLOCK, &block, nullptr);
  DropInheritedFds(pipe_fd);

  static Helper h;  // zero-initialised, and too big for the stack
  h.pipe_fd = 3;
  h.parent = parent;
  // Failure means the spawner is already gone (ESRCH): go straight to
  // the drain.
  h.parent_alive = WatchParent(h);
  while (h.parent_alive) {
    if (WaitOnce(h, -1) == 0 && !ReadRegistrations(h)) {
      break;  // EOF: only possible once the spawner is dead
    }
  }
  // Drain late registrations until every write end is closed.
  for (;;) {
    const int r = WaitOnce(h, kDrainTimeoutMs);
    if (r < 0 || (r == 0 && !ReadRegistrations(h))) {
      break;
    }
  }
  for (int i = 0; i < h.count; i++) {
    kill(h.entries[i].pid, h.entries[i].sig);
  }
  _exit(0);
}

void Write(const Registration& r) {
  ssize_t n;
  do {
    n = write(helper_fd, &r, sizeof(r));
  } while (n < 0 && errno == EINTR);
}

}  // namespace

bool ParentDeathHelper::Enabled() {
#ifdef __linux__
  return getenv("CUTTLEFISH_PARENT_DEATH_HELPER") != nullptr;
#else
  return true;
#endif
}

Result<void> ParentDeathHelper::Ensure() {
  std::lock_guard lock(ensure_mutex);
  const pid_t self = getpid();
  if (helper_fd >= 0 && helper_owner == self) {
    return {};
  }
  if (helper_fd >= 0) {
    // Inherited across a fork without exec. That process's helper must
    // see EOF once we are done registering with it, so let go now.
    close(helper_fd);
    helper_fd = -1;
  }
  int fds[2];
  CF_EXPECTF(pipe(fds) == 0, "pipe failed: {}", StrError());
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  const pid_t pid = fork();
  if (pid == 0) {
    // The analyzer follows this into the helper's blocking reads and
    // believes they happen under ensure_mutex; the child is alone.
    HelperMain(fds[0], self);
  }
  close(fds[0]);
  if (pid < 0) {
    close(fds[1]);
    return CF_ERRF("fork failed: {}", StrError());
  }
  helper_fd = fds[1];
  helper_owner = self;
  return {};
}

void ParentDeathHelper::RegisterSelfPreExec(int sig) {
  if (helper_fd >= 0) {
    Write({getpid(), sig});
  }
}

void ParentDeathHelper::Forget(pid_t pid) {
  if (helper_fd >= 0 && helper_owner == getpid()) {
    Write({pid, 0});
  }
}

}  // namespace cuttlefish
