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

// OpenBSD implementation of proc_file_utils.h. OpenBSD has no procfs; process
// information is retrieved through sysctl(2) (kern.proc and kern.proc_args).

#include <errno.h>
#include <limits.h>
#include <paths.h>
#include <stdlib.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "absl/log/log.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "android-base/file.h"

#include "cuttlefish/posix/strerror.h"
#include "cuttlefish/process/proc_file_utils.h"
#include "cuttlefish/result/result.h"

namespace cuttlefish {
namespace {

Result<struct kinfo_proc> KinfoProc(const pid_t pid) {
  struct kinfo_proc kp = {};
  size_t size = sizeof(kp);
  int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid, sizeof(kp), 1};
  CF_EXPECTF(sysctl(mib, 6, &kp, &size, nullptr, 0) == 0,
             "sysctl(kern.proc.pid.{}) failed: {}", pid, StrError(errno));
  CF_EXPECTF(size == sizeof(kp), "No process with pid {}", pid);
  return kp;
}

// Retrieves one of the KERN_PROC_ARGV / KERN_PROC_ENV string vectors.
Result<std::vector<std::string>> ProcStrings(const pid_t pid, const int op) {
  int mib[] = {CTL_KERN, KERN_PROC_ARGS, pid, op};
  std::vector<char> buffer;
  for (size_t size = 16 * 1024; size <= 2 * ARG_MAX; size *= 2) {
    buffer.resize(size);
    size_t length = size;
    if (sysctl(mib, 4, buffer.data(), &length, nullptr, 0) == 0) {
      std::vector<std::string> result;
      for (char** entry = reinterpret_cast<char**>(buffer.data());
           *entry != nullptr; entry++) {
        result.emplace_back(*entry);
      }
      return result;
    }
    CF_EXPECTF(errno == ENOMEM, "sysctl(kern.proc_args.{}) failed: {}", pid,
               StrError(errno));
  }
  return CF_ERRF("sysctl(kern.proc_args.{}) keeps running out of space", pid);
}

// Resolves a possibly relative argv[0] the way execvp(3) did, relative to the
// current working directory of the process.
Result<std::string> ResolveArgv0(const pid_t pid, const std::string& argv0) {
  CF_EXPECT(!argv0.empty(), "Empty argv[0]");
  std::string candidate = argv0;
  if (candidate.find('/') == std::string::npos) {
    const char* path_env = getenv("PATH");
    for (std::string_view dir :
         absl::StrSplit(path_env ? path_env : _PATH_DEFPATH, ':')) {
      std::string attempt =
          absl::StrCat(dir.empty() ? "." : dir, "/", candidate);
      if (access(attempt.c_str(), X_OK) == 0) {
        candidate = attempt;
        break;
      }
    }
  }
  if (candidate.front() != '/') {
    int mib[] = {CTL_KERN, KERN_PROC_CWD, pid};
    char cwd[PATH_MAX] = {};
    size_t size = sizeof(cwd);
    if (sysctl(mib, 3, cwd, &size, nullptr, 0) == 0 && cwd[0] != '\0') {
      candidate = std::string(cwd) + "/" + candidate;
    }
  }
  std::string resolved;
  if (android::base::Realpath(candidate, &resolved)) {
    return resolved;
  }
  return candidate;
}

}  // namespace

Result<std::vector<pid_t>> CollectPids(const uid_t uid) {
  int mib[] = {CTL_KERN,
               KERN_PROC,
               KERN_PROC_RUID,
               static_cast<int>(uid),
               sizeof(struct kinfo_proc),
               0};
  size_t size = 0;
  CF_EXPECTF(sysctl(mib, 6, nullptr, &size, nullptr, 0) == 0,
             "sysctl(kern.proc.ruid.{}) failed: {}", uid, StrError(errno));
  std::vector<struct kinfo_proc> procs;
  for (int attempt = 0; attempt < 4; attempt++) {
    // Leave room for processes created between the two calls.
    procs.resize(size / sizeof(struct kinfo_proc) + 16);
    size = procs.size() * sizeof(struct kinfo_proc);
    mib[5] = procs.size();
    if (sysctl(mib, 6, procs.data(), &size, nullptr, 0) == 0) {
      procs.resize(size / sizeof(struct kinfo_proc));
      std::vector<pid_t> pids;
      for (const struct kinfo_proc& kp : procs) {
        // Only report the main thread of each process, like /proc on Linux.
        if (kp.p_tid != -1 && kp.p_tid != kp.p_pid) {
          continue;
        }
        pids.push_back(kp.p_pid);
      }
      return pids;
    }
    CF_EXPECTF(errno == ENOMEM, "sysctl(kern.proc.ruid.{}) failed: {}", uid,
               StrError(errno));
  }
  return CF_ERRF("sysctl(kern.proc.ruid.{}) keeps running out of space", uid);
}

Result<std::vector<std::string>> GetCmdArgs(const pid_t pid) {
  CF_EXPECT_EQ(CF_EXPECT(OwnerUid(pid)), getuid());
  return CF_EXPECT(ProcStrings(pid, KERN_PROC_ARGV));
}

Result<std::string> GetExecutablePath(const pid_t pid) {
  // There is no equivalent of /proc/<pid>/exe; the best approximation is the
  // argv[0] the process was started with.
  std::vector<std::string> argv = CF_EXPECT(ProcStrings(pid, KERN_PROC_ARGV));
  CF_EXPECTF(!argv.empty(), "Process {} has no arguments", pid);
  return CF_EXPECT(ResolveArgv0(pid, argv.front()));
}

Result<std::vector<pid_t>> CollectPidsByExecName(const std::string& exec_name,
                                                 const uid_t uid) {
  CF_EXPECT_EQ(android::base::Basename(exec_name), exec_name);
  std::vector<pid_t> output_pids;
  for (const pid_t pid : CF_EXPECT(CollectPids(uid))) {
    Result<struct kinfo_proc> kp = KinfoProc(pid);
    if (!kp.has_value() || kp->p_ruid != uid) {
      VLOG(1) << "Process #" << pid << " does not belong to " << uid;
      continue;
    }
    // p_comm is the executable's basename, truncated to _MAXCOMLEN - 1.
    std::string_view comm(kp->p_comm);
    std::string_view name(exec_name);
    if (name.size() >= sizeof(kp->p_comm)) {
      name = name.substr(0, sizeof(kp->p_comm) - 1);
    }
    if (comm == name) {
      output_pids.push_back(pid);
    }
  }
  return output_pids;
}

Result<std::vector<pid_t>> CollectPidsByExecPath(const std::string& exec_path,
                                                 const uid_t uid) {
  std::vector<pid_t> output_pids;
  for (const pid_t pid : CF_EXPECT(CollectPids(uid))) {
    Result<std::string> pid_exec_path = GetExecutablePath(pid);
    if (pid_exec_path.has_value() && *pid_exec_path == exec_path) {
      output_pids.push_back(pid);
    }
  }
  return output_pids;
}

Result<std::vector<pid_t>> CollectPidsByArgv0(const std::string& expected_argv0,
                                              const uid_t uid) {
  std::vector<pid_t> output_pids;
  for (const pid_t pid : CF_EXPECT(CollectPids(uid))) {
    Result<std::vector<std::string>> argv = GetCmdArgs(pid);
    if (argv.has_value() && !argv->empty() && argv->front() == expected_argv0) {
      output_pids.push_back(pid);
    }
  }
  return output_pids;
}

Result<uid_t> OwnerUid(const pid_t pid) {
  return CF_EXPECT(KinfoProc(pid)).p_ruid;
}

Result<std::unordered_map<std::string, std::string>> GetEnvs(const pid_t pid) {
  uid_t owner = CF_EXPECT(OwnerUid(pid));
  CF_EXPECT(getuid() == owner, "Owned by another user of uid" << owner);
  std::unordered_map<std::string, std::string> envs;
  for (const std::string& entry : CF_EXPECT(ProcStrings(pid, KERN_PROC_ENV))) {
    envs.emplace(absl::StrSplit(entry, absl::MaxSplits('=', 1)));
  }
  return envs;
}

Result<ProcInfo> ExtractProcInfo(const pid_t pid) {
  struct kinfo_proc kp = CF_EXPECT(KinfoProc(pid));
  return ProcInfo{.pid_ = pid,
                  .real_owner_ = kp.p_ruid,
                  .effective_owner_ = kp.p_uid,
                  .actual_exec_path_ = CF_EXPECT(GetExecutablePath(pid)),
                  .envs_ = CF_EXPECT(GetEnvs(pid)),
                  .args_ = CF_EXPECT(GetCmdArgs(pid))};
}

Result<pid_t> Ppid(const pid_t pid) { return CF_EXPECT(KinfoProc(pid)).p_ppid; }

}  // namespace cuttlefish
