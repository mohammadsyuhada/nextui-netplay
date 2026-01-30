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
static FileList* file_list = NULL;

// Current netplay state (verified via hash comparison)
static NetplayState current_state = NETPLAY_STATE_DISABLED;
static bool version_supported = false;

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
    version_supported = FileOps_isVersionSupported(system_version);

    if (version_supported) {
        current_state = FileOps_verifyState(system_version, file_list);
        // If unknown, fall back to saved state
        if (current_state == NETPLAY_STATE_UNKNOWN) {
            current_state = FileOps_getState();
        }
    } else {
        current_state = NETPLAY_STATE_DISABLED;
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

    // Apply patched files
    if (!FileOps_applyPatched(system_version, file_list)) {
        strncpy(error_message, "Failed to apply patched files.", sizeof(error_message) - 1);
        app_state = STATE_ERROR;
        return;
    }

    FileOps_saveState(NETPLAY_STATE_ENABLED);
    FileOps_saveInstalledVersion(system_version);

    // Refresh state
    refresh_state();
    app_state = STATE_MENU;
}

// Disable netplay operation
static void do_disable_netplay(void) {
    const char* installed_ver = FileOps_getInstalledVersion();
    if (!installed_ver || strlen(installed_ver) == 0) {
        // Try using current system version
        installed_ver = system_version;
    }

    if (!FileOps_restoreOriginals(installed_ver, file_list)) {
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
    const SelfUpdateStatus* status = SelfUpdate_getStatus();
    SelfUpdateState state = status->state;

    if (PAD_justPressed(BTN_A)) {
        if (state == SELFUPDATE_STATE_COMPLETED) {
            // Restart the app
            char launch_path[600];
            snprintf(launch_path, sizeof(launch_path), "%s/launch.sh", pak_path);
            execl("/bin/sh", "sh", launch_path, NULL);
            quit = true;  // If exec fails
        }
    }
    else if (PAD_justPressed(BTN_B)) {
        if (state == SELFUPDATE_STATE_DOWNLOADING) {
            SelfUpdate_cancelUpdate();
        }
        if (state == SELFUPDATE_STATE_IDLE || state == SELFUPDATE_STATE_ERROR ||
            state == SELFUPDATE_STATE_COMPLETED) {
            app_state = STATE_ABOUT;
            *dirty = 1;
        }
    }

    // Always redraw during update to show progress
    if (state == SELFUPDATE_STATE_DOWNLOADING || state == SELFUPDATE_STATE_EXTRACTING ||
        state == SELFUPDATE_STATE_APPLYING || state == SELFUPDATE_STATE_CHECKING) {
        *dirty = 1;
    }
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

    // Get system version
    if (!Config_getSystemVersion(system_version, sizeof(system_version))) {
        strcpy(system_version, "Unknown");
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
            FileOps_saveInstalledVersion(system_version);
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
