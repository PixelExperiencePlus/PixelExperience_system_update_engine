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

#include "update_engine/postinstall_runner_action.h"

#include <stdlib.h>
#include <sys/mount.h>
#include <vector>

#include <base/bind.h>

#include "update_engine/action_processor.h"
#include "update_engine/subprocess.h"
#include "update_engine/utils.h"

namespace chromeos_update_engine {

using std::string;
using std::vector;

namespace {
// The absolute path to the post install command.
const char kPostinstallScript[] = "/postinst";

// Path to the binary file used by kPostinstallScript. Used to get and log the
// file format of the binary to debug issues when the ELF format on the update
// doesn't match the one on the current system. This path is not executed.
const char kDebugPostinstallBinaryPath[] = "/usr/bin/cros_installer";
}

void PostinstallRunnerAction::PerformAction() {
  CHECK(HasInputObject());
  install_plan_ = GetInputObject();
  const string install_device = install_plan_.install_path;
  ScopedActionCompleter completer(processor_, this);

  // Make mountpoint.
  TEST_AND_RETURN(
      utils::MakeTempDirectory("au_postint_mount.XXXXXX", &temp_rootfs_dir_));
  ScopedDirRemover temp_dir_remover(temp_rootfs_dir_);

  const string mountable_device =
      utils::MakePartitionNameForMount(install_device);
  if (mountable_device.empty()) {
    LOG(ERROR) << "Cannot make mountable device from " << install_device;
    return;
  }

  if (!utils::MountFilesystem(mountable_device, temp_rootfs_dir_, MS_RDONLY))
    return;

  LOG(INFO) << "Performing postinst with install device " << install_device
            << " and mountable device " << mountable_device;

  temp_dir_remover.set_should_remove(false);
  completer.set_should_complete(false);

  if (install_plan_.powerwash_required) {
    if (utils::CreatePowerwashMarkerFile(powerwash_marker_file_)) {
      powerwash_marker_created_ = true;
    } else {
      completer.set_code(ErrorCode::kPostinstallPowerwashError);
      return;
    }
  }

  // Logs the file format of the postinstall script we are about to run. This
  // will help debug when the postinstall script doesn't match the architecture
  // of our build.
  LOG(INFO) << "Format file for new " <<  kPostinstallScript << " is: "
            << utils::GetFileFormat(temp_rootfs_dir_ + kPostinstallScript);
  LOG(INFO) << "Format file for new " <<  kDebugPostinstallBinaryPath << " is: "
            << utils::GetFileFormat(
                temp_rootfs_dir_ + kDebugPostinstallBinaryPath);

  // Runs the postinstall script asynchronously to free up the main loop while
  // it's running.
  vector<string> command;
  if (!install_plan_.download_url.empty()) {
    command.push_back(temp_rootfs_dir_ + kPostinstallScript);
  } else {
    // TODO(sosa): crbug.com/366207.
    // If we're doing a rollback, just run our own postinstall.
    command.push_back(kPostinstallScript);
  }
  command.push_back(install_device);
  if (!Subprocess::Get().Exec(command,
                              base::Bind(
                                  &PostinstallRunnerAction::CompletePostinstall,
                                  base::Unretained(this)))) {
    CompletePostinstall(1, "Postinstall didn't launch");
  }
}

void PostinstallRunnerAction::CompletePostinstall(int return_code,
                                                  const string& output) {
  ScopedActionCompleter completer(processor_, this);
  ScopedTempUnmounter temp_unmounter(temp_rootfs_dir_);

  bool success = true;

  if (return_code != 0) {
    LOG(ERROR) << "Postinst command failed with code: " << return_code;
    success = false;
  }

  // We only attempt to mark the new slot as active if the /postinst script
  // succeeded.
  if (success && !system_state_->boot_control()->SetActiveBootSlot(
        install_plan_.target_slot)) {
    success = false;
  }

  if (!success) {
    LOG(ERROR) << "Postinstall action failed.";

    // Undo any changes done to trigger Powerwash using clobber-state.
    if (powerwash_marker_created_)
      utils::DeletePowerwashMarkerFile(powerwash_marker_file_);

    if (return_code == 3) {
      // This special return code means that we tried to update firmware,
      // but couldn't because we booted from FW B, and we need to reboot
      // to get back to FW A.
      completer.set_code(ErrorCode::kPostinstallBootedFromFirmwareB);
    }

    if (return_code == 4) {
      // This special return code means that we tried to update firmware,
      // but couldn't because we booted from FW B, and we need to reboot
      // to get back to FW A.
      completer.set_code(ErrorCode::kPostinstallFirmwareRONotUpdatable);
    }

    return;
  }

  LOG(INFO) << "Postinst command succeeded";
  if (HasOutputPipe()) {
    SetOutputObject(install_plan_);
  }

  completer.set_code(ErrorCode::kSuccess);
}

}  // namespace chromeos_update_engine