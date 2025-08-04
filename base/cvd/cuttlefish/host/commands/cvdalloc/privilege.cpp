/*
 * Copyright (C) 2025 The Android Open Source Project
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
#include "cuttlefish/host/commands/cvdalloc/privilege.h"

#include <errno.h>

#if defined(__linux__)
#include <linux/capability.h>
#include <linux/prctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

#include <android-base/logging.h>

#if defined(__linux__)
int SetAmbientCapabilities() {
  /*
   * If we're on Linux, try and set capabilities instead of using setuid.
   * We need capability CAP_NET_ADMIN, but this won't normally persist
   * through exec when we shell out to invoke network commands.
   * Instead, we need to set this as an ambient capability.
   *
   * For portability reasons, run the syscall by hand and not drag in a
   * libcap dependency into bzlmod which is hard to conditionalize.
   *
   * TODO: Maybe this is neater with implementing this with netlink.
   */
  struct __user_cap_header_struct h = {_LINUX_CAPABILITY_VERSION_3, getpid()};
  struct __user_cap_data_struct data[2];

  int r = syscall(SYS_capget, &h, data);
  if (r == -1) {
    LOG(INFO) << "SYS_capget: " << strerror(errno);
    return -1;
  }

  data[0].inheritable = data[0].permitted;
  r = syscall(SYS_capset, &h, data);
  if (r == -1) {
    LOG(INFO) << "SYS_capset: " << strerror(errno);
    return -1;
  }

  r = prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_NET_ADMIN, 0, 0);
  if (r == -1) {
    LOG(INFO) << "prctl: " << strerror(errno);
    return -1;
  }

  return 1;
}
#endif

int DropPrivileges(uid_t orig) {
#if defined(__linux__)
  struct __user_cap_header_struct h = {_LINUX_CAPABILITY_VERSION_3, getpid()};
  struct __user_cap_data_struct data[2];
  data[0] = {0, 0, 0};
  data[1] = {0, 0, 0};
  int r = syscall(SYS_capset, &h, data);
  if (r == -1) {
    LOG(INFO) << "SYS_capset: " << strerror(errno);
    return -1;
  }

  r = prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0L, 0L, 0L);
  if (r == -1) {
    LOG(INFO) << "prctl: " << strerror(errno);
    return -1;
  }
#endif

  return setuid(orig);
}
