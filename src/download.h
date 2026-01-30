#ifndef __DOWNLOAD_H__
#define __DOWNLOAD_H__

#include <stdbool.h>

// Download module states
typedef enum {
    DOWNLOAD_STATE_IDLE = 0,
    DOWNLOAD_STATE_CHECKING,
    DOWNLOAD_STATE_DOWNLOADING,
    DOWNLOAD_STATE_EXTRACTING,
    DOWNLOAD_STATE_COMPLETED,
    DOWNLOAD_STATE_ERROR
} DownloadState;

// Download status
typedef struct {
    DownloadState state;
    int progress_percent;
    char status_message[256];
    char error_message[256];
} DownloadStatus;

// Initialize download module
// pak_path: path to the .pak directory
void Download_init(const char* pak_path);

// Cleanup resources
void Download_cleanup(void);

// Check if internet is available
bool Download_checkInternet(void);

// Check if a version is supported (has release assets)
// version: NextUI version string (e.g., "NextUI-20250115-1")
// platform: platform name (e.g., "tg5040")
bool Download_isVersionSupported(const char* version, const char* platform);

// Get download URL for the netplay assets
// Returns URL or NULL if not found
// Caller must free the returned string
char* Download_getAssetUrl(const char* version, const char* platform);

// Start download in background thread
// version: NextUI version string
// platform: platform name
// dest_dir: destination directory for extracted files
// Returns 0 on success, -1 if already running
int Download_start(const char* version, const char* platform, const char* dest_dir);

// Cancel ongoing download
void Download_cancel(void);

// Get current status
const DownloadStatus* Download_getStatus(void);

// Update function (call from main loop)
void Download_update(void);

// Check if download is running
bool Download_isRunning(void);

#endif
