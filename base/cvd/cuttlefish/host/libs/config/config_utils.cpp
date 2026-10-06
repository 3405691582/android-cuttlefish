/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "cuttlefish/host/libs/config/config_utils.h"

#include <unistd.h>

#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

#include "absl/log/log.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"

#include "cuttlefish/common/libs/utils/contains.h"
#include "cuttlefish/common/libs/utils/environment.h"
#include "cuttlefish/common/libs/utils/host_info.h"
#include "cuttlefish/common/libs/utils/in_sandbox.h"
#include "cuttlefish/common/libs/utils/random.h"
#include "cuttlefish/files/file_exists.h"
#include "cuttlefish/host/libs/config/config_constants.h"
#include "cuttlefish/process/execute.h"

namespace cuttlefish {

int InstanceFromString(std::string instance_str) {
  if (absl::StartsWith(instance_str, kVsocUserPrefix)) {
    instance_str = instance_str.substr(std::string(kVsocUserPrefix).size());
  } else if (absl::StartsWith(instance_str, kCvdNamePrefix)) {
    instance_str = instance_str.substr(std::string(kCvdNamePrefix).size());
  }

  int instance = std::stoi(instance_str);
  if (instance <= 0) {
    LOG(INFO) << "Failed to interpret \"" << instance_str << "\" as an id, "
              << "using instance id " << kDefaultInstance;
    return kDefaultInstance;
  }
  return instance;
}

int InstanceFromEnvironment() {
  std::string instance_str = StringFromEnv(kCuttlefishInstanceEnvVarName, "");
  if (instance_str.empty()) {
    // Try to get it from the user instead
    instance_str = StringFromEnv("USER", "");

    if (instance_str.empty()) {
      VLOG(0) << kCuttlefishInstanceEnvVarName
              << " and USER unset, using instance id " << kDefaultInstance;
      return kDefaultInstance;
    }
    if (!absl::StartsWith(instance_str, kVsocUserPrefix)) {
      // No user or we don't recognize this user
      VLOG(0) << "Non-vsoc user, using instance id " << kDefaultInstance;
      return kDefaultInstance;
    }
  }
  return InstanceFromString(instance_str);
}

int GetInstance() {
  static int instance_id = InstanceFromEnvironment();
  return instance_id;
}

int GetDefaultVsockCid() {
  // we assume that this function is used to configure CuttlefishConfig once
  static const int default_vsock_cid = 3 + GetInstance() - 1;
  return default_vsock_cid;
}

int GetVsockServerPort(
    const int base, const int vsock_guest_cid /**< per instance guest cid */) {
  return base + (vsock_guest_cid - 3);
}

std::string GetGlobalConfigFileLink() {
  return StringFromEnv("HOME", ".") + "/.cuttlefish_config.json";
}

std::string ForCurrentInstance(const char* prefix) {
  std::ostringstream stream;
  stream << prefix << std::setfill('0') << std::setw(2) << GetInstance();
  return stream.str();
}

std::string RandomSerialNumber(const std::string& prefix) {
  const std::string hex_characters = "0123456789ABCDEF";
  return prefix + GenerateRandomString(hex_characters, 10);
}

std::string DefaultHostArtifactsPath(const std::string& file_name) {
  return (StringFromEnv("ANDROID_HOST_OUT", StringFromEnv("HOME", ".")) + "/") +
         file_name;
}

std::string HostBinaryDir() { return DefaultHostArtifactsPath("bin"); }

namespace {

// The bin directory of the system QEMU: the first directory in PATH, then in
// the usual package prefixes, that holds qemu-img (which ships with every QEMU
// and does not depend on the architecture); /usr/bin as before otherwise.
std::string SystemQemuBinaryDir() {
  std::string search = StringFromEnv("PATH", "");
  absl::StrAppend(&search, ":/usr/local/bin:/usr/pkg/bin:/opt/homebrew/bin");
  for (std::string_view dir : absl::StrSplit(search, ':', absl::SkipEmpty())) {
    if (access(absl::StrCat(dir, "/qemu-img").c_str(), X_OK) == 0) {
      return std::string(dir);
    }
  }
  return "/usr/bin";
}

}  // namespace

bool UseQemuPrebuilt() {
#if !defined(__linux__) && !defined(__APPLE__)
  // The host package only carries QEMU prebuilts for Linux hosts.
  return false;
#else
  const std::string target_prod_str = StringFromEnv("TARGET_PRODUCT", "");
  if (!Contains(target_prod_str, "arm")) {
    return true;
  }
  return false;
#endif
}

std::string DefaultQemuBinaryDir() {
  if (UseQemuPrebuilt()) {
    return HostBinaryDir() + "/" + HostArchStr() + "-linux-gnu/qemu";
  }
  return SystemQemuBinaryDir();
}

std::string HostBinaryPath(const std::string& binary_name) {
#ifdef __ANDROID__
  return binary_name;
#else
  return HostBinaryDir() + "/" + binary_name;
#endif
}

std::string HostUsrSharePath(const std::string& file) {
  return DefaultHostArtifactsPath("usr/share/" + file);
}

std::string HostQemuBiosPath() {
  if (UseQemuPrebuilt()) {
    return DefaultHostArtifactsPath("usr/share/qemu/" + HostArchStr() +
                                    "-linux-gnu");
  }
  // QEMU installs its firmware next to its binaries: <prefix>/bin and
  // <prefix>/share/qemu.
  const std::string bin_dir = SystemQemuBinaryDir();
  if (absl::EndsWith(bin_dir, "/bin")) {
    return bin_dir.substr(0, bin_dir.size() - 4) + "/share/qemu";
  }
  return "/usr/share/qemu";
}

std::string DefaultGuestImagePath(const std::string& file_name) {
  return (StringFromEnv("ANDROID_PRODUCT_OUT", StringFromEnv("HOME", "."))) +
         file_name;
}

std::string DefaultEnvironmentPath(const std::string& environment_key,
                                   const std::string& default_value,
                                   const std::string& subpath) {
  return StringFromEnv(environment_key, default_value) + "/" + subpath;
}

bool IsValidAndroidHostOutPath(const std::string& path) {
  std::string internal_start_bin_path = path + "/bin/cvd_internal_start";
  std::string launch_cvd_bin_path = path + "/bin/launch_cvd";
  return FileExists(internal_start_bin_path) || FileExists(launch_cvd_bin_path);
}

// In practice this is mostly validating that the `cuttlefish-base` debian
// package is installed, which implies that more things are present like the
// predefined network setup.
bool HostSupportsQemuCli() {
  static bool supported =
#ifdef __linux__
      InSandbox() ||
      Execute({"/usr/lib/cuttlefish-common/bin/capability_query.py",
               "qemu_cli"}) == 0;
#else
      true;
#endif
  return supported;
}

std::string GetSeccompPolicyDir() {
  std::string kSeccompDir =
      "usr/share/crosvm/" + HostArchStr() + "-linux-gnu/seccomp";
  return DefaultHostArtifactsPath(kSeccompDir);
}
}  // namespace cuttlefish
