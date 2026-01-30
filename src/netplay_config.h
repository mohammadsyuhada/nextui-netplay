#ifndef __NETPLAY_CONFIG_H__
#define __NETPLAY_CONFIG_H__

#include <stdbool.h>

// GitHub repository (format: "owner/repo")
#define NETPLAY_GITHUB_REPO "mohammadsyuhada/nextui-netplay"

// System version file
#define VERSION_FILE_PATH "/mnt/SDCARD/.system/version.txt"

// File list structure
typedef struct {
    char** files;
    int count;
} FileList;

// Load file list from configuration
// Returns allocated FileList on success, NULL on error
// Caller must free with Config_freeFiles()
FileList* Config_loadFiles(const char* conf_path);

// Free file list
void Config_freeFiles(FileList* list);

// Get NextUI version from system
// Returns version string (e.g., "NextUI-20250115-1")
// buffer must be at least 64 bytes
bool Config_getSystemVersion(char* buffer, int buffer_size);

#endif
