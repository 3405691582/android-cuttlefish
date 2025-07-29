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
#include <unistd.h>

#include <android-base/logging.h>

#include "allocd/alloc_utils.h"
#include "cuttlefish/common/libs/fs/shared_fd.h"

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

ABSL_FLAG(std::string, id, "", "Id");
ABSL_FLAG(std::string, teardown_event_socket, "", "Socket");
ABSL_FLAG(std::string, allocate_event_socket, "", "Socket");
ABSL_FLAG(std::string, config, "", "Configuration file");

int main(int argc, char *argv[]) {
  std::vector<char *> args = absl::ParseCommandLine(argc, argv);

  uid_t orig = getuid();
  int id = std::stoi(absl::GetFlag(FLAGS_id));
  std::string if_id = absl::StrFormat("%02d", id);

  pid_t pid = fork();
  if (pid < 0) return -1;

  /*
   * Explicit setuid calls seem to be required.
   *
   * This likely has something to do with invoking external commands,
   * but it isn't clear why an explicit setuid(0) is necessary.
   * TODO: We should just use capabilities on Linux and rely on setuid as
   * a fallback for other platforms.
   */

  if (pid > 0) {
    int r = setuid(0);
    if (r == -1) {
      LOG(ERROR) << "Couldn't setuid root: " << strerror(errno);
      return 1;
    }

    LOG(INFO) << "cvdalloc: allocating network resources";

    cuttlefish::CreateBridge("cvd-br");

    if (!cuttlefish::CreateMobileIface(absl::StrCat("cvd-mtap-", if_id), id,
                                       cuttlefish::kMobileIp)) {
      LOG(ERROR) << "cvdalloc: couldn't create interface";
    }
    if (!cuttlefish::CreateMobileIface(absl::StrCat("cvd-wtap-", if_id), id,
                                       cuttlefish::kWirelessIp)) {
      LOG(ERROR) << "cvdalloc: couldn't create interface";
    }
    if (!cuttlefish::CreateEthernetIface(absl::StrCat("cvd-etap-", if_id),
                                         "cvd-br", true, true, false)) {
      LOG(ERROR) << "cvdalloc: couldn't create interface";
    }

    r = setuid(orig);
    if (r == -1) {
      LOG(WARNING) << "cvdalloc: couldn't drop privileges";
      return 1;
    }

    auto sock = cuttlefish::SharedFD::Dup(
        std::stoi(absl::GetFlag(FLAGS_allocate_event_socket)));
    if (!sock->IsOpen()) {
      LOG(ERROR) << "cvdalloc: allocate socket is closed: " << sock->StrError();
      return 1;
    }
    sock->Shutdown(SHUT_RDWR);

    return 0;
  }

  if (pid == 0) {
    LOG(INFO) << "cvdalloc (teardown): waiting to teardown";

    auto sock = cuttlefish::SharedFD::Dup(
        std::stoi(absl::GetFlag(FLAGS_teardown_event_socket)));
    if (!sock->IsOpen()) {
      LOG(ERROR) << "cvdalloc (teardown): teardown socket is closed: "
                 << sock->StrError();
      return 1;
    }

    int i;
    if (sock->Read(&i, sizeof(i)) < 0) {
      sock->Close();
      return 1;
    }

    int r = setuid(0);
    if (r == -1) {
      LOG(ERROR) << "Couldn't setuid root: " << strerror(errno);
      return 1;
    }

    LOG(INFO) << "cvdalloc (teardown): tearing down resources";

    if (!cuttlefish::DestroyMobileIface(absl::StrCat("cvd-mtap-", if_id), id,
                                        cuttlefish::kMobileIp)) {
      LOG(ERROR) << "cvdalloc (teardown): couldn't destroy interface";
    }
    if (!cuttlefish::DestroyMobileIface(absl::StrCat("cvd-wtap-", if_id), id,
                                        cuttlefish::kWirelessIp)) {
      LOG(ERROR) << "cvdalloc (teardown): couldn't destroy interface";
    }
    if (!cuttlefish::DestroyEthernetIface(absl::StrCat("cvd-etap-", if_id),
                                          true, true, false)) {
      LOG(ERROR) << "cvdalloc (teardown): couldn't destroy interface";
    }

    cuttlefish::DestroyBridge("cvd-br");

    r = setuid(orig);
    if (r == -1) {
      LOG(WARNING) << "cvdalloc: couldn't drop privileges";
      return 1;
    }

    return 0;
  }
}
