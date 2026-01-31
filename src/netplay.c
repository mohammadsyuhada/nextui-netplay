#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/stat.h>
#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "utils.h"

#include "netplay_config.h"
#include "fileops.h"
#include "ui.h"
#include "selfupdate.h"

// App states
typedef enum {
    STATE_MENU = 0,
    STATE_CONFIRM_ENABLE,
    STATE_CONFIRM_DISABLE,
    STATE_SUPPORTED,
    STATE_ABOUT,
    STATE_UPDATING,
    STATE_ERROR
} AppState;

// Global state
static bool quit = false;
static AppState app_state = STATE_MENU;
static SDL_Surface* screen;

// Menu navigation
static int menu_selected = 0;
static int supported_scroll = 0;

// Platform and version info
static char pak_path[512] = "";
static char platform[32] = "";
static char system_version[64] = "";
static char system_commit[32] = "";
static FileList* file_list = NULL;

// Current netplay state (verified via hash comparison)
static NetplayState current_state = NETPLAY_STATE_DISABLED;
static bool version_supported = false;

// Compatible version info (when using backward compatibility)
// Made non-static so UI can access them
char compatible_version[64] = "";
char compatible_commit[32] = "";
bool using_compatible_version = false;

// Error message buffer
static char error_message[256] = "";

static void sigHandler(int sig) {
    switch (sig) {
    case SIGINT:
    case SIGTERM:
        quit = true;
        break;
    default:
        break;
    }
}

// Get current working directory as pak path
static void get_pak_path(void) {
    char cwd[512];
    if (getcwd(cwd, sizeof(cwd))) {
        strncpy(pak_path, cwd, sizeof(pak_path) - 1);
    } else {
        strcpy(pak_path, ".");
    }
}

// Get platform from environment or default
static void get_platform(void) {
    const char* env_platform = getenv("PLATFORM");
    if (env_platform && strlen(env_platform) > 0) {
        strncpy(platform, env_platform, sizeof(platform) - 1);
    } else {
        // Default based on UNION_PLATFORM
        const char* union_platform = getenv("UNION_PLATFORM");
        if (union_platform && strlen(union_platform) > 0) {
            strncpy(platform, union_platform, sizeof(platform) - 1);
        } else {
            strcpy(platform, "tg5040");
        }
    }
}

// Refresh current state by verifying file hashes
static void refresh_state(void) {
    using_compatible_version = false;
    compatible_version[0] = '\0';
    compatible_commit[0] = '\0';

    // First check if exact version+commit is supported
    version_supported = FileOps_isVersionSupported(system_version, system_commit);

    if (version_supported) {
        // Exact version match - verify state
        current_state = FileOps_verifyState(system_version, system_commit, file_list);

        // If unknown (files don't match either patched or original),
        // this likely means system was updated. Default to DISABLED (safer than false positive)
        if (current_state == NETPLAY_STATE_UNKNOWN) {
            current_state = NETPLAY_STATE_DISABLED;
        }
    } else {
        // Check installed version - maybe system was updated and files need restoration
        const char* installed = FileOps_getInstalledVersion();
        if (installed && strlen(installed) > 0) {
            // Parse the installed version
            char inst_ver[64], inst_commit[32];
            if (FileOps_parseInstalledVersion(installed, inst_ver, sizeof(inst_ver), inst_commit, sizeof(inst_commit))) {
                // Check if this installed version's files can be used
                NetplayState inst_state = FileOps_verifyState(inst_ver, inst_commit, file_list);
                if (inst_state == NETPLAY_STATE_ENABLED) {
                    // System files still match the installed patched version
                    current_state = NETPLAY_STATE_ENABLED;
                    // Use this version for disable operation
                    strncpy(compatible_version, inst_ver, sizeof(compatible_version) - 1);
                    strncpy(compatible_commit, inst_commit, sizeof(compatible_commit) - 1);
                    using_compatible_version = true;
                    version_supported = true;  // Can at least disable
                    return;
                }
            }
        }

        // No exact match - try backward compatibility
        // Look for a version whose original files match current system
        if (FileOps_findCompatibleVersion(file_list, compatible_version, sizeof(compatible_version),
                                          compatible_commit, sizeof(compatible_commit))) {
            version_supported = true;
            using_compatible_version = true;
            current_state = NETPLAY_STATE_DISABLED;  // Original files match, so not patched
        } else {
            // No compatible version found
            version_supported = false;
            current_state = NETPLAY_STATE_DISABLED;
        }
    }
}

// Enable netplay operation
static void do_enable_netplay(void) {
    if (!version_supported) {
        snprintf(error_message, sizeof(error_message),
            "Version %s not supported.\nUpdate the Netplay pak.", system_version);
        app_state = STATE_ERROR;
        return;
    }

    // Determine which version to use for patching
    const char* use_version = using_compatible_version ? compatible_version : system_version;
    const char* use_commit = using_compatible_version ? compatible_commit : system_commit;

    // Apply patched files
    if (!FileOps_applyPatched(use_version, use_commit, file_list)) {
        strncpy(error_message, "Failed to apply patched files.", sizeof(error_message) - 1);
        app_state = STATE_ERROR;
        return;
    }

    FileOps_saveState(NETPLAY_STATE_ENABLED);

    // Save full version identifier (version-commit)
    char version_id[128];
    snprintf(version_id, sizeof(version_id), "%s-%s", use_version, use_commit);
    FileOps_saveInstalledVersion(version_id);

    // Refresh state
    refresh_state();
    app_state = STATE_MENU;
}

// Disable netplay operation
static void do_disable_netplay(void) {
    char use_version[64] = "";
    char use_commit[32] = "";

    // First try to use the installed version info
    const char* installed = FileOps_getInstalledVersion();
    if (installed && strlen(installed) > 0) {
        FileOps_parseInstalledVersion(installed, use_version, sizeof(use_version), use_commit, sizeof(use_commit));
    }

    // Fallback to compatible version if detected
    if (strlen(use_version) == 0 && using_compatible_version) {
        strncpy(use_version, compatible_version, sizeof(use_version) - 1);
        strncpy(use_commit, compatible_commit, sizeof(use_commit) - 1);
    }

    // Final fallback to current system version
    if (strlen(use_version) == 0) {
        strncpy(use_version, system_version, sizeof(use_version) - 1);
        strncpy(use_commit, system_commit, sizeof(use_commit) - 1);
    }

    if (strlen(use_commit) == 0) {
        strncpy(error_message, "Cannot determine version to restore.\nCommit hash unknown.", sizeof(error_message) - 1);
        app_state = STATE_ERROR;
        return;
    }

    if (!FileOps_restoreOriginals(use_version, use_commit, file_list)) {
        strncpy(error_message, "Failed to restore original files.", sizeof(error_message) - 1);
        app_state = STATE_ERROR;
        return;
    }

    FileOps_saveState(NETPLAY_STATE_DISABLED);
    FileOps_saveInstalledVersion("");

    // Refresh state
    refresh_state();
    app_state = STATE_MENU;
}

// Track if update check was running
static bool update_check_was_running = false;

// Track if autosleep is disabled (during update)
static bool autosleep_disabled = false;

// Handle menu state input
static void handle_menu_input(int* dirty) {
    // Check if update check just completed (to update menu label)
    SelfUpdateState update_state = SelfUpdate_getState();
    bool update_checking = (update_state == SELFUPDATE_STATE_CHECKING);
    if (update_check_was_running && !update_checking) {
        *dirty = 1;  // Redraw to show update status in menu
    }
    update_check_was_running = update_checking;

    if (PAD_justPressed(BTN_UP)) {
        if (menu_selected > 0) {
            menu_selected--;
            *dirty = 1;
        }
    }
    else if (PAD_justPressed(BTN_DOWN)) {
        if (menu_selected < MENU_ITEM_COUNT - 1) {
            menu_selected++;
            *dirty = 1;
        }
    }
    else if (PAD_justPressed(BTN_A)) {
        switch (menu_selected) {
            case MENU_TOGGLE:
                if (current_state == NETPLAY_STATE_ENABLED) {
                    app_state = STATE_CONFIRM_DISABLE;
                } else if (version_supported) {
                    app_state = STATE_CONFIRM_ENABLE;
                }
                break;
            case MENU_SUPPORTED:
                supported_scroll = 0;
                app_state = STATE_SUPPORTED;
                break;
            case MENU_ABOUT:
                app_state = STATE_ABOUT;
                break;
        }
        *dirty = 1;
    }
    else if (PAD_justPressed(BTN_B)) {
        quit = true;
    }
}

// Handle supported cores screen input
static void handle_supported_input(int* dirty) {
    if (PAD_justPressed(BTN_UP)) {
        if (supported_scroll > 0) {
            supported_scroll--;
            *dirty = 1;
        }
    }
    else if (PAD_justPressed(BTN_DOWN)) {
        // Allow scrolling (8 cores total, max visible depends on screen)
        if (supported_scroll < 4) {  // Simple limit
            supported_scroll++;
            *dirty = 1;
        }
    }
    else if (PAD_justPressed(BTN_B)) {
        app_state = STATE_MENU;
        *dirty = 1;
    }
}

// Handle about screen input
static void handle_about_input(int* dirty) {
    const SelfUpdateStatus* status = SelfUpdate_getStatus();

    if (PAD_justPressed(BTN_A)) {
        if (status->update_available) {
            // Start the update
            if (SelfUpdate_startUpdate() == 0) {
                app_state = STATE_UPDATING;
                *dirty = 1;
            }
        }
    }
    else if (PAD_justPressed(BTN_B)) {
        app_state = STATE_MENU;
        *dirty = 1;
    }

    // Check if update check completed
    SelfUpdateState update_state = SelfUpdate_getState();
    if (update_state == SELFUPDATE_STATE_IDLE || update_state == SELFUPDATE_STATE_ERROR) {
        *dirty = 1;  // Refresh to show update status
    }
}

// Handle update progress screen input
static void handle_updating_input(int* dirty) {
    // Disable autosleep during update to prevent screen turning off
    if (!autosleep_disabled) {
        PWR_disableAutosleep();
        autosleep_disabled = true;
    }

    const SelfUpdateStatus* status = SelfUpdate_getStatus();
    SelfUpdateState state = status->state;

    if (PAD_justPressed(BTN_A)) {
        if (state == SELFUPDATE_STATE_COMPLETED) {
            // Close the app - user will relaunch manually
            quit = true;
        }
    }
    else if (PAD_justPressed(BTN_B)) {
        if (state == SELFUPDATE_STATE_DOWNLOADING) {
            SelfUpdate_cancelUpdate();
        }
        if (state == SELFUPDATE_STATE_IDLE || state == SELFUPDATE_STATE_ERROR ||
            state == SELFUPDATE_STATE_COMPLETED) {
            // Re-enable autosleep when leaving update screen
            if (autosleep_disabled) {
                PWR_enableAutosleep();
                autosleep_disabled = false;
            }
            app_state = STATE_ABOUT;
            *dirty = 1;
        }
    }

    // Always redraw while on update screen to show progress and state changes
    *dirty = 1;
}

int main(int argc, char* argv[]) {
    // Initialize settings
    InitSettings();
    screen = GFX_init(MODE_MAIN);
    PAD_init();
    PWR_init();

    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);

    // Get pak path and platform
    get_pak_path();
    get_platform();

    // Get system version and commit
    if (!Config_getSystemVersion(system_version, sizeof(system_version))) {
        strcpy(system_version, "Unknown");
    }
    if (!Config_getSystemCommit(system_commit, sizeof(system_commit))) {
        strcpy(system_commit, "");
    }

    // Load file list from configuration
    char conf_path[600];
    snprintf(conf_path, sizeof(conf_path), "%s/files.conf", pak_path);
    file_list = Config_loadFiles(conf_path);
    if (!file_list) {
        // Use default file list
        file_list = calloc(1, sizeof(FileList));
        file_list->count = 3;
        file_list->files = calloc(3, sizeof(char*));
        file_list->files[0] = strdup("bin/minarch.elf");
        file_list->files[1] = strdup("cores/gambatte_libretro.so");
        file_list->files[2] = strdup("cores/gpsp_libretro.so");
    }

    // Initialize modules
    FileOps_init(pak_path, platform);
    UI_init();
    SelfUpdate_init(pak_path);

    // Check for updates on startup
    SelfUpdate_checkForUpdate();

    // Verify actual state by comparing file hashes
    refresh_state();

    // Sync state file with verified state
    if (current_state != NETPLAY_STATE_UNKNOWN) {
        FileOps_saveState(current_state);
        if (current_state == NETPLAY_STATE_ENABLED) {
            // Save full version identifier
            const char* use_version = using_compatible_version ? compatible_version : system_version;
            const char* use_commit = using_compatible_version ? compatible_commit : system_commit;
            char version_id[128];
            snprintf(version_id, sizeof(version_id), "%s-%s", use_version, use_commit);
            FileOps_saveInstalledVersion(version_id);
        }
    }

    int dirty = 1;
    int show_setting = 0;

    while (!quit) {
        uint32_t frame_start = SDL_GetTicks();
        PAD_poll();

        // Handle input based on state
        switch (app_state) {
            case STATE_MENU:
                handle_menu_input(&dirty);
                break;

            case STATE_CONFIRM_ENABLE:
                if (PAD_justPressed(BTN_A)) {
                    do_enable_netplay();
                    dirty = 1;
                }
                else if (PAD_justPressed(BTN_B)) {
                    app_state = STATE_MENU;
                    dirty = 1;
                }
                break;

            case STATE_CONFIRM_DISABLE:
                if (PAD_justPressed(BTN_A)) {
                    do_disable_netplay();
                    dirty = 1;
                }
                else if (PAD_justPressed(BTN_B)) {
                    app_state = STATE_MENU;
                    dirty = 1;
                }
                break;

            case STATE_SUPPORTED:
                handle_supported_input(&dirty);
                break;

            case STATE_ABOUT:
                handle_about_input(&dirty);
                break;

            case STATE_UPDATING:
                handle_updating_input(&dirty);
                break;

            case STATE_ERROR:
                if (PAD_justPressed(BTN_B) || PAD_justPressed(BTN_A)) {
                    app_state = STATE_MENU;
                    dirty = 1;
                }
                break;
        }

        PWR_update(&dirty, &show_setting, NULL, NULL);

        if (dirty) {
            switch (app_state) {
                case STATE_MENU:
                    UI_renderMenu(screen, show_setting, menu_selected,
                                  current_state, version_supported);
                    break;
                case STATE_CONFIRM_ENABLE:
                    UI_renderConfirm(screen, show_setting, "Enable Netplay",
                        "This will replace system files with netplay-enabled versions.\n\nContinue?");
                    break;
                case STATE_CONFIRM_DISABLE:
                    UI_renderConfirm(screen, show_setting, "Disable Netplay",
                        "This will restore original system files.\n\nContinue?");
                    break;
                case STATE_SUPPORTED:
                    UI_renderSupported(screen, show_setting, supported_scroll);
                    break;
                case STATE_ABOUT:
                    UI_renderAbout(screen, show_setting);
                    break;
                case STATE_UPDATING:
                    UI_renderUpdateProgress(screen, show_setting);
                    break;
                case STATE_ERROR:
                    UI_renderError(screen, show_setting, error_message);
                    break;
            }

            dirty = 0;
        } else {
            GFX_sync();
        }
    }

    // Re-enable autosleep if it was disabled
    if (autosleep_disabled) {
        PWR_enableAutosleep();
        autosleep_disabled = false;
    }

    // Cleanup
    SelfUpdate_cleanup();
    UI_quit();
    Config_freeFiles(file_list);

    QuitSettings();
    PWR_quit();
    PAD_quit();
    GFX_quit();

    return EXIT_SUCCESS;
}
