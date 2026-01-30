#ifndef __SELFUPDATE_H__
#define __SELFUPDATE_H__

#include <stdbool.h>

// GitHub repository (format: "owner/repo")
#define APP_GITHUB_REPO "mohammadsyuhada/nextui-netplay"

// Release asset name pattern (the .pak.zip file)
#define APP_RELEASE_ASSET "Netplay.pak.zip"

// Fallback version if version file not found
#define APP_VERSION_FALLBACK "0.0.0"

// Self-update module states
typedef enum {
    SELFUPDATE_STATE_IDLE = 0,
    SELFUPDATE_STATE_CHECKING,      // Checking for updates
    SELFUPDATE_STATE_DOWNLOADING,   // Downloading update
    SELFUPDATE_STATE_EXTRACTING,    // Extracting ZIP
    SELFUPDATE_STATE_APPLYING,      // Moving files in place
    SELFUPDATE_STATE_COMPLETED,     // Ready for restart
    SELFUPDATE_STATE_ERROR
} SelfUpdateState;

// Update status information
typedef struct {
    SelfUpdateState state;
    bool update_available;
    char current_version[32];
    char latest_version[32];
    char download_url[512];
    char release_notes[1024];       // Release description from GitHub
    int progress_percent;           // 0-100
    char status_message[256];
    char error_message[256];
} SelfUpdateStatus;

// Initialize self-update module
// pak_path: path to the .pak directory
// Reads version from state/app_version.txt
// Returns 0 on success, -1 on error
int SelfUpdate_init(const char* pak_path);

// Cleanup resources
void SelfUpdate_cleanup(void);

// Get current app version
const char* SelfUpdate_getVersion(void);

// Check for updates (non-blocking, runs in background thread)
// Returns 0 if check started, -1 if already running
int SelfUpdate_checkForUpdate(void);

// Start the update process (download + extract + apply)
// Returns 0 if update started, -1 if already running or no update available
int SelfUpdate_startUpdate(void);

// Cancel ongoing update
void SelfUpdate_cancelUpdate(void);

// Get current status
const SelfUpdateStatus* SelfUpdate_getStatus(void);

// Update function (call from main loop)
void SelfUpdate_update(void);

// Check if restart is required to apply update
bool SelfUpdate_isPendingRestart(void);

// Get current state
SelfUpdateState SelfUpdate_getState(void);

#endif
