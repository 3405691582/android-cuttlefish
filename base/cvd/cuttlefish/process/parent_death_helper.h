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

#pragma once

#include <sys/types.h>

#include "cuttlefish/result/result.h"

namespace cuttlefish {

/*
 * Emulates PR_SET_PDEATHSIG where the kernel has no such thing: a
 * forked helper process per spawner watches the spawner's exit and
 * then signals every child that registered with it.
 *
 * The helper is forked lazily by Ensure(); children register
 * themselves between fork and exec with RegisterSelfPreExec(), which
 * mirrors prctl(2) and closes the fork-then-spawner-dies race: the
 * helper drains registrations to EOF before it signals anybody. A
 * child's exit is watched too, so a reaped pid that gets reused is
 * never signalled; Forget() after a reap just tidies up earlier.
 *
 * On Linux the helper is off unless CUTTLEFISH_PARENT_DEATH_HELPER is
 * set in the environment, which lets the Linux test suite exercise it.
 */
class ParentDeathHelper {
 public:
  // Whether this process uses the helper instead of the kernel.
  static bool Enabled();

  // Forks the helper if this process does not have one yet. Call
  // before forking a child that will register.
  static Result<void> Ensure();

  // Child side, between fork and exec. Async-signal-safe; a no-op if
  // the spawner has no helper.
  static void RegisterSelfPreExec(int sig);

  // Spawner side, after reaping `pid`.
  static void Forget(pid_t pid);
};

}  // namespace cuttlefish
