/*
 * Copyright (C) 2016-2021 The UBports Project
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
 *
 * Authored by: Marius Gripsgard <mariogrip@ubports.com>
 */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#include <install/install.h>
#include <recovery_utils/roots.h>
#include <recovery_ui/ui.h>

static const char *UBUNTU_COMMAND_FILE = "/cache/recovery/ubuntu_command";
static const char *UBUNTU_UPDATE_SCRIPT = "/system/bin/system-image-upgrader";

int setup_partitions(RecoveryUI *ui) {
    // Map logical partitions so there is a device node to put into fstab
    map_logical_partitions();

    if (setup_install_mounts() != 0) {
        ui->Print("Failed to set up expected mounts for install; aborting\n");
        return INSTALL_ERROR;
    }

    // updating volume table including /etc/fstab, which is used by our script as
    // we are doing manual mounts where we only specify the path.
    load_volume_table();

    return INSTALL_SUCCESS;
}

void show_installation_error(RecoveryUI *ui, int result) {
    // Enable text to show errors to the user
    ui->ShowText(true);
    ui->Print("Error installing Ubuntu update, exit code: %d\n", result);
    ui->Print("Please go to Advanced -> View recovery logs -> /cache/ubuntu_updater.log\n");

    // TODO: Show error ui instead of text only?
    ui->SetProgressType(RecoveryUI::EMPTY);
}

InstallResult do_ubuntu_update(RecoveryUI *ui){
    // Disable text because otherwise the animation is not showing
    ui->ShowText(false);

    ui->Print("Setting up partitions...\n");
    int result = setup_partitions(ui);
    if (result != INSTALL_SUCCESS) {
        show_installation_error(ui, result);
        return INSTALL_ERROR;
    }

    ui->Print("Executing Ubuntu update script...\n");
    ui->SetBackground(RecoveryUI::INSTALLING_UPDATE);

    char tmp[PATH_MAX];
    sprintf(tmp, "%s %s 2>&1 > /cache/ubuntu_updater.log", UBUNTU_UPDATE_SCRIPT, UBUNTU_COMMAND_FILE);
    char buf[128];
    FILE *pout = popen(tmp, "r");
    if (!pout) {
        show_installation_error(ui, 1);
        return INSTALL_ERROR;
    }
    while (fgets(buf, 128, pout) != NULL) {
        float progress;
        int stage, max_stage;
        char title[128];
        if (sscanf(buf, "progress:%f", &progress)) {
            ui->ShowProgress(progress, 2.0);
        } else if (sscanf(buf, "title:%[^\n]", title)) {
            ui->SetProgressText(title);
        } else if (sscanf(buf, "stage:%d:%d", &stage, &max_stage)) {
            ui->SetStage(stage, max_stage);
        }
    }
    ui->SetProgressText("");
    ui->SetStage(-1, -1);
    result = fclose(pout);
    if (result != 0) {
        show_installation_error(ui, result);
        return INSTALL_ERROR;
    }

    for (int i = 5; i > 0; i--) {
        char rtxt[50];
        sprintf(rtxt, "Rebooting in %d...", i);
        ui->SetProgressText(rtxt);
        sleep(1);
    }
    ui->SetProgressText("");

    // ui->SetEnableReboot(true);
    ui->Print("\n");
    return INSTALL_NONE;
    // return INSTALL_SUCCESS;
}

int do_test_update(RecoveryUI *ui){
    // Disable text because otherwise the animation is not showing
    ui->ShowText(false);

    ui->Print("Executing Ubuntu update script...\n");
    ui->SetBackground(RecoveryUI::INSTALLING_UPDATE);
    ui->SetProgressType(RecoveryUI::INDETERMINATE);

    // Wait for 10 seconds to showcase the awesome install animation
    sleep(10);

    // Enable text, which stops the install animation
    ui->ShowText(true);
    ui->Print("... not really though, this is just a test.\n");

    char tmp[PATH_MAX];
    sprintf(tmp, "%s %s &> /cache/ubuntu_updater.log", UBUNTU_UPDATE_SCRIPT, UBUNTU_COMMAND_FILE);
    ui->Print("\nNormally this would call ->\n%s\n", tmp);

    ui->Print("\nCouting down from 5 instead.\n");
    ui->Print("5");
    sleep(1);
    ui->Print(" 4");
    sleep(1);
    ui->Print(" 3");
    sleep(1);
    ui->Print(" 2");
    sleep(1);
    ui->Print(" 1");
    sleep(1);
    ui->Print("\n");

    ui->Print("\nDone with counting, have a nice day!\n");

    return INSTALL_ERROR;
}
