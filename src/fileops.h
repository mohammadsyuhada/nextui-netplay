#ifndef __FILEOPS_H__
#define __FILEOPS_H__

#include <stdbool.h>
#include "netplay_config.h"

// Netplay state
typedef enum {
    NETPLAY_STATE_UNKNOWN = 0,
    NETPLAY_STATE_DISABLED,
    NETPLAY_STATE_ENABLED
} NetplayState;

// Initialize file operations module
// pak_path: path to the .pak directory
// platform: platform name (e.g., "tg5040")
void FileOps_init(const char* pak_path, const char* platform);

// Get current netplay state
NetplayState FileOps_getState(void);

// Save netplay state
void FileOps_saveState(NetplayState state);

// Get installed version (if any)
// Returns version string or empty string if not installed
const char* FileOps_getInstalledVersion(void);

// Save installed version
void FileOps_saveInstalledVersion(const char* version);

// Check if version is supported (files exist in bin/{version}-{platform}/)
bool FileOps_isVersionSupported(const char* version);

// Apply patched files from bundled pak
// version: system version string
// files: list of files to apply
// Returns true on success
bool FileOps_applyPatched(const char* version, FileList* files);

// Restore original files from bundled pak
// version: version that was used to enable (stored in installed_version.txt)
// files: list of files to restore
// Returns true on success
bool FileOps_restoreOriginals(const char* version, FileList* files);

// Verify if patched files are actually installed by comparing file hashes
// version: version to check against
// files: list of files to verify
// Returns: NETPLAY_STATE_ENABLED if patched files match,
//          NETPLAY_STATE_DISABLED if original files match,
//          NETPLAY_STATE_UNKNOWN if files don't match either
NetplayState FileOps_verifyState(const char* version, FileList* files);

// Get system directory path
const char* FileOps_getSystemDir(void);

#endif
