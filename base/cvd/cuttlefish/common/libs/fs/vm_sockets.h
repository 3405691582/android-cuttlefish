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
#pragma once

// vsock (AF_VSOCK) only exists on Linux. On other platforms the well-known
// address constants are still provided so that code that only formats vsock
// addresses into command lines or uses them as default arguments compiles;
// the AF_VSOCK socket constructors in UniqueFd/SharedFD fail at runtime with
// EAFNOSUPPORT there. The vhost-user-vsock transport (a unix socket speaking
// the vhost-device-vsock "connect <port>" protocol) does not need AF_VSOCK
// and works everywhere.

#ifdef __linux__
// Must be included after <sys/socket.h> to support older libc.
#include <linux/vm_sockets.h>  // IWYU pragma: export
#include <sys/socket.h>        // IWYU pragma: keep
#else
#define VMADDR_CID_ANY (-1U)
#define VMADDR_PORT_ANY (-1U)
#define VMADDR_CID_HYPERVISOR 0U
#define VMADDR_CID_LOCAL 1U
#define VMADDR_CID_HOST 2U
#endif
