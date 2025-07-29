/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "cuttlefish/host/commands/assemble_cvd/alloc.h"

#include <iomanip>
#include <sstream>

#include "cuttlefish/common/libs/fs/shared_fd.h"

namespace cuttlefish {

static std::string StrForInstance(const std::string& prefix, int num) {
  std::ostringstream stream;
  stream << prefix << std::setfill('0') << std::setw(2) << num;
  return stream.str();
}

IfaceConfig DefaultNetworkInterfaces(int num) {
  IfaceConfig config{};
  config.mobile_tap.name = StrForInstance("cvd-mtap-", num);
  config.mobile_tap.resource_id = 0;
  config.mobile_tap.session_id = 0;

  config.bridged_wireless_tap.name = StrForInstance("cvd-wtap-", num);
  config.bridged_wireless_tap.resource_id = 0;
  config.bridged_wireless_tap.session_id = 0;

  config.non_bridged_wireless_tap.name = StrForInstance("cvd-wifiap-", num);
  config.non_bridged_wireless_tap.resource_id = 0;
  config.non_bridged_wireless_tap.session_id = 0;

  config.ethernet_tap.name = StrForInstance("cvd-etap-", num);
  config.ethernet_tap.resource_id = 0;
  config.ethernet_tap.session_id = 0;

  return config;
}

} // namespace cuttlefish
