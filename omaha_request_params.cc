//
// Copyright (C) 2011 The Android Open Source Project
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
//

#include "update_engine/omaha_request_params.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/utsname.h>

#include <map>
#include <string>
#include <vector>

#include <base/files/file_util.h>
#include <base/strings/string_util.h>
#include <chromeos/key_value_store.h>
#include <policy/device_policy.h>

#include "update_engine/constants.h"
#include "update_engine/hardware_interface.h"
#include "update_engine/platform_constants.h"
#include "update_engine/system_state.h"
#include "update_engine/utils.h"

#define CALL_MEMBER_FN(object, member) ((object).*(member))

using std::map;
using std::string;
using std::vector;

namespace chromeos_update_engine {

const char OmahaRequestParams::kAppId[] =
    "{87efface-864d-49a5-9bb3-4b050a7c227a}";
const char OmahaRequestParams::kOsVersion[] = "Indy";
const char OmahaRequestParams::kUpdateChannelKey[] = "CHROMEOS_RELEASE_TRACK";
const char OmahaRequestParams::kIsPowerwashAllowedKey[] =
    "CHROMEOS_IS_POWERWASH_ALLOWED";
const char OmahaRequestParams::kAutoUpdateServerKey[] = "CHROMEOS_AUSERVER";

const char* kChannelsByStability[] = {
    // This list has to be sorted from least stable to most stable channel.
    "canary-channel",
    "dev-channel",
    "beta-channel",
    "stable-channel",
};

bool OmahaRequestParams::Init(const string& in_app_version,
                              const string& in_update_url,
                              bool in_interactive) {
  LOG(INFO) << "Initializing parameters for this update attempt";
  InitFromLsbValue();
  bool stateful_override = !ShouldLockDown();
  os_platform_ = constants::kOmahaPlatformName;
  os_version_ = OmahaRequestParams::kOsVersion;
  app_version_ = in_app_version.empty() ?
      GetLsbValue("CHROMEOS_RELEASE_VERSION", "", nullptr, stateful_override) :
      in_app_version;
  os_sp_ = app_version_ + "_" + GetMachineType();
  os_board_ = GetLsbValue("CHROMEOS_RELEASE_BOARD",
                          "",
                          nullptr,
                          stateful_override);
  string release_app_id = GetLsbValue("CHROMEOS_RELEASE_APPID",
                                      OmahaRequestParams::kAppId,
                                      nullptr,
                                      stateful_override);
  board_app_id_ = GetLsbValue("CHROMEOS_BOARD_APPID",
                              release_app_id,
                              nullptr,
                              stateful_override);
  canary_app_id_ = GetLsbValue("CHROMEOS_CANARY_APPID",
                               release_app_id,
                               nullptr,
                               stateful_override);
  app_lang_ = "en-US";
  hwid_ = system_state_->hardware()->GetHardwareClass();
  if (CollectECFWVersions()) {
    fw_version_ = system_state_->hardware()->GetFirmwareVersion();
    ec_version_ = system_state_->hardware()->GetECVersion();
  }

  if (current_channel_ == target_channel_) {
    // deltas are only okay if the /.nodelta file does not exist.  if we don't
    // know (i.e. stat() returns some unexpected error), then err on the side of
    // caution and say deltas are not okay.
    struct stat stbuf;
    delta_okay_ = (stat((root_ + "/.nodelta").c_str(), &stbuf) < 0) &&
                  (errno == ENOENT);

  } else {
    LOG(INFO) << "Disabling deltas as a channel change is pending";
    // For now, disable delta updates if the current channel is different from
    // the channel that we're sending to the update server because such updates
    // are destined to fail -- the current rootfs hash will be different than
    // the expected hash due to the different channel in /etc/lsb-release.
    delta_okay_ = false;
  }

  if (in_update_url.empty())
    update_url_ = GetLsbValue(kAutoUpdateServerKey,
                              constants::kOmahaDefaultProductionURL,
                              nullptr, stateful_override);
  else
    update_url_ = in_update_url;

  // Set the interactive flag accordingly.
  interactive_ = in_interactive;
  return true;
}

bool OmahaRequestParams::IsUpdateUrlOfficial() const {
  return (update_url_ == constants::kOmahaDefaultAUTestURL ||
          update_url_ == GetLsbValue(kAutoUpdateServerKey,
                                     constants::kOmahaDefaultProductionURL,
                                     nullptr, !ShouldLockDown()));
}

bool OmahaRequestParams::CollectECFWVersions() const {
  return base::StartsWithASCII(hwid_, string("SAMS ALEX"), true) ||
         base::StartsWithASCII(hwid_, string("BUTTERFLY"), true) ||
         base::StartsWithASCII(hwid_, string("LUMPY"), true) ||
         base::StartsWithASCII(hwid_, string("PARROT"), true) ||
         base::StartsWithASCII(hwid_, string("SPRING"), true) ||
         base::StartsWithASCII(hwid_, string("SNOW"), true);
}

bool OmahaRequestParams::SetTargetChannel(const string& new_target_channel,
                                          bool is_powerwash_allowed) {
  LOG(INFO) << "SetTargetChannel called with " << new_target_channel
            << ", Is Powerwash Allowed = "
            << utils::ToString(is_powerwash_allowed)
            << ". Current channel = " << current_channel_
            << ", existing target channel = " << target_channel_
            << ", download channel = " << download_channel_;
  TEST_AND_RETURN_FALSE(IsValidChannel(new_target_channel));
  chromeos::KeyValueStore lsb_release;
  base::FilePath kFile(root_ + kStatefulPartition + "/etc/lsb-release");

  lsb_release.Load(kFile);
  lsb_release.SetString(kUpdateChannelKey, new_target_channel);
  lsb_release.SetBoolean(kIsPowerwashAllowedKey, is_powerwash_allowed);

  TEST_AND_RETURN_FALSE(base::CreateDirectory(kFile.DirName()));
  TEST_AND_RETURN_FALSE(lsb_release.Save(kFile));
  target_channel_ = new_target_channel;
  is_powerwash_allowed_ = is_powerwash_allowed;
  return true;
}

void OmahaRequestParams::SetTargetChannelFromLsbValue() {
  string target_channel_new_value = GetLsbValue(
      kUpdateChannelKey,
      current_channel_,
      &chromeos_update_engine::OmahaRequestParams::IsValidChannel,
      true);  // stateful_override

  if (target_channel_ != target_channel_new_value) {
    target_channel_ = target_channel_new_value;
    LOG(INFO) << "Target Channel set to " << target_channel_
              << " from LSB file";
  }
}

void OmahaRequestParams::SetCurrentChannelFromLsbValue() {
  string current_channel_new_value = GetLsbValue(
      kUpdateChannelKey,
      current_channel_,
      nullptr,  // No need to validate the read-only rootfs channel.
      false);  // stateful_override is false so we get the current channel.

  if (current_channel_ != current_channel_new_value) {
    current_channel_ = current_channel_new_value;
    LOG(INFO) << "Current Channel set to " << current_channel_
              << " from LSB file in rootfs";
  }
}

void OmahaRequestParams::SetIsPowerwashAllowedFromLsbValue() {
  string is_powerwash_allowed_str = GetLsbValue(
      kIsPowerwashAllowedKey,
      "false",
      nullptr,  // no need to validate
      true);  // always get it from stateful, as that's the only place it'll be
  bool is_powerwash_allowed_new_value = (is_powerwash_allowed_str == "true");
  if (is_powerwash_allowed_ != is_powerwash_allowed_new_value) {
    is_powerwash_allowed_ = is_powerwash_allowed_new_value;
    LOG(INFO) << "Powerwash Allowed set to "
              << utils::ToString(is_powerwash_allowed_)
              << " from LSB file in stateful";
  }
}

void OmahaRequestParams::UpdateDownloadChannel() {
  if (download_channel_ != target_channel_) {
    download_channel_ = target_channel_;
    LOG(INFO) << "Download channel for this attempt = " << download_channel_;
  }
}

void OmahaRequestParams::InitFromLsbValue() {
  SetCurrentChannelFromLsbValue();
  SetTargetChannelFromLsbValue();
  SetIsPowerwashAllowedFromLsbValue();
  UpdateDownloadChannel();
}

string OmahaRequestParams::GetLsbValue(const string& key,
                                       const string& default_value,
                                       ValueValidator validator,
                                       bool stateful_override) const {
  vector<string> files;
  if (stateful_override) {
    files.push_back(string(kStatefulPartition) + "/etc/lsb-release");
  }
  files.push_back("/etc/lsb-release");
  for (vector<string>::const_iterator it = files.begin();
       it != files.end(); ++it) {
    // TODO(adlr): make sure files checked are owned as root (and all their
    // parents are recursively, too).
    chromeos::KeyValueStore data;
    if (!data.Load(base::FilePath(root_ + *it)))
      continue;

    string value;
    if (data.GetString(key, &value)) {
      if (validator && !CALL_MEMBER_FN(*this, validator)(value)) {
        continue;
      }
      return value;
    }
  }
  // not found
  return default_value;
}

string OmahaRequestParams::GetMachineType() const {
  struct utsname buf;
  string ret;
  if (uname(&buf) == 0)
    ret = buf.machine;
  return ret;
}

bool OmahaRequestParams::ShouldLockDown() const {
  if (force_lock_down_) {
    return forced_lock_down_;
  }
  return system_state_->hardware()->IsOfficialBuild() &&
            system_state_->hardware()->IsNormalBootMode();
}

bool OmahaRequestParams::IsValidChannel(const string& channel) const {
  return GetChannelIndex(channel) >= 0;
}

void OmahaRequestParams::set_root(const string& root) {
  root_ = root;
  InitFromLsbValue();
}

void OmahaRequestParams::SetLockDown(bool lock) {
  force_lock_down_ = true;
  forced_lock_down_ = lock;
}

int OmahaRequestParams::GetChannelIndex(const string& channel) const {
  for (size_t t = 0; t < arraysize(kChannelsByStability); ++t)
    if (channel == kChannelsByStability[t])
      return t;

  return -1;
}

bool OmahaRequestParams::to_more_stable_channel() const {
  int current_channel_index = GetChannelIndex(current_channel_);
  int download_channel_index = GetChannelIndex(download_channel_);

  return download_channel_index > current_channel_index;
}

string OmahaRequestParams::GetAppId() const {
  return download_channel_ == "canary-channel" ? canary_app_id_ : board_app_id_;
}

}  // namespace chromeos_update_engine