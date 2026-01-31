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
// Returns full version identifier (version-commit) or empty string if not installed
const char* FileOps_getInstalledVersion(void);

// Save installed version with commit hash
// version_id: full version identifier in format "version-commit"
void FileOps_saveInstalledVersion(const char* version_id);

// Parse installed version to extract version and commit components
// full: full version identifier (e.g., "NextUI-20260130-0-7d98d7f7")
// version_out: buffer to store version (e.g., "NextUI-20260130-0")
// commit_out: buffer to store commit (e.g., "7d98d7f7")
// Returns true if parsing succeeded
bool FileOps_parseInstalledVersion(const char* full, char* version_out, int version_size, char* commit_out, int commit_size);

// Check if version+commit is supported (files exist in bin/{version}-{commit}-{platform}/)
bool FileOps_isVersionSupported(const char* version, const char* commit);

// Apply patched files from bundled pak
// version: system version string
// commit: system commit hash
// files: list of files to apply
// Returns true on success
bool FileOps_applyPatched(const char* version, const char* commit, FileList* files);

// Restore original files from bundled pak
// version: version that was used to enable
// commit: commit hash that was used to enable
// files: list of files to restore
// Returns true on success
bool FileOps_restoreOriginals(const char* version, const char* commit, FileList* files);

// Verify if patched files are actually installed by comparing file hashes
// version: version to check against
// commit: commit hash to check against
// files: list of files to verify
// Returns: NETPLAY_STATE_ENABLED if patched files match,
//          NETPLAY_STATE_DISABLED if original files match,
//          NETPLAY_STATE_UNKNOWN if files don't match either
NetplayState FileOps_verifyState(const char* version, const char* commit, FileList* files);

// Find a compatible version from available patches
// Scans bin/ for any version whose original files match current system files
// files: list of files to check
// version_out: buffer to store found version string (e.g., "NextUI-20260130-0")
// commit_out: buffer to store found commit hash (e.g., "7d98d7f7")
// Returns true if a compatible version was found
bool FileOps_findCompatibleVersion(FileList* files, char* version_out, int version_size, char* commit_out, int commit_size);

// Get system directory path
const char* FileOps_getSystemDir(void);

#endif
