#include "fileops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

// Paths
static char pak_path[512] = "";
static char platform[32] = "";
static char state_file[600] = "";
static char version_file[600] = "";
static char system_dir[600] = "";
static char installed_version[64] = "";

// Helper to get basename from path
static const char* get_basename(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// Compare two files byte-by-byte
// Returns: 0 if identical, 1 if different, -1 on error
static int compare_files(const char* file1, const char* file2) {
    FILE* f1 = fopen(file1, "rb");
    FILE* f2 = fopen(file2, "rb");

    if (!f1 || !f2) {
        if (f1) fclose(f1);
        if (f2) fclose(f2);
        return -1;
    }

    // Compare file sizes first
    fseek(f1, 0, SEEK_END);
    fseek(f2, 0, SEEK_END);
    long size1 = ftell(f1);
    long size2 = ftell(f2);

    if (size1 != size2) {
        fclose(f1);
        fclose(f2);
        return 1;  // Different sizes
    }

    // Compare contents
    fseek(f1, 0, SEEK_SET);
    fseek(f2, 0, SEEK_SET);

    char buf1[4096];
    char buf2[4096];
    size_t bytes_read;

    while ((bytes_read = fread(buf1, 1, sizeof(buf1), f1)) > 0) {
        if (fread(buf2, 1, bytes_read, f2) != bytes_read) {
            fclose(f1);
            fclose(f2);
            return 1;
        }
        if (memcmp(buf1, buf2, bytes_read) != 0) {
            fclose(f1);
            fclose(f2);
            return 1;  // Different content
        }
    }

    fclose(f1);
    fclose(f2);
    return 0;  // Identical
}

void FileOps_init(const char* path, const char* plat) {
    if (!path || !plat) return;

    strncpy(pak_path, path, sizeof(pak_path) - 1);
    strncpy(platform, plat, sizeof(platform) - 1);

    // Set up paths
    snprintf(state_file, sizeof(state_file), "%s/state/netplay.state", pak_path);
    snprintf(version_file, sizeof(version_file), "%s/state/installed_version.txt", pak_path);
    snprintf(system_dir, sizeof(system_dir), "/mnt/SDCARD/.system/%s", platform);

    // Create state directory if needed
    char state_dir[600];
    snprintf(state_dir, sizeof(state_dir), "%s/state", pak_path);
    mkdir(state_dir, 0755);

    // Read installed version
    installed_version[0] = '\0';
    FILE* f = fopen(version_file, "r");
    if (f) {
        if (fgets(installed_version, sizeof(installed_version), f)) {
            char* nl = strchr(installed_version, '\n');
            if (nl) *nl = '\0';
        }
        fclose(f);
    }
}

NetplayState FileOps_getState(void) {
    FILE* f = fopen(state_file, "r");
    if (!f) return NETPLAY_STATE_DISABLED;

    char buf[32] = "";
    if (fgets(buf, sizeof(buf), f)) {
        char* nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
    }
    fclose(f);

    if (strcmp(buf, "enabled") == 0) {
        return NETPLAY_STATE_ENABLED;
    }
    return NETPLAY_STATE_DISABLED;
}

void FileOps_saveState(NetplayState state) {
    FILE* f = fopen(state_file, "w");
    if (!f) return;

    if (state == NETPLAY_STATE_ENABLED) {
        fprintf(f, "enabled\n");
    } else {
        fprintf(f, "disabled\n");
    }
    fclose(f);
}

const char* FileOps_getInstalledVersion(void) {
    return installed_version;
}

void FileOps_saveInstalledVersion(const char* version) {
    if (!version) return;

    strncpy(installed_version, version, sizeof(installed_version) - 1);

    FILE* f = fopen(version_file, "w");
    if (f) {
        fprintf(f, "%s\n", version);
        fclose(f);
    }
}

bool FileOps_isVersionSupported(const char* version) {
    if (!version || strlen(version) == 0) return false;

    // Check if bin/{version}-{platform}/ directory exists
    char version_dir[600];
    snprintf(version_dir, sizeof(version_dir), "%s/bin/%s-%s", pak_path, version, platform);

    return (access(version_dir, F_OK) == 0);
}

bool FileOps_applyPatched(const char* version, FileList* files) {
    if (!version || !files || files->count == 0) return false;

    char cmd[1024];

    // Source directory: bin/{version}-{platform}/patched/
    char patched_dir[600];
    snprintf(patched_dir, sizeof(patched_dir), "%s/bin/%s-%s/patched", pak_path, version, platform);

    for (int i = 0; i < files->count; i++) {
        char src_path[600];
        char dst_path[600];

        // Source: just the basename (e.g., minarch.elf)
        const char* basename = get_basename(files->files[i]);
        snprintf(src_path, sizeof(src_path), "%s/%s", patched_dir, basename);

        // Destination: full path in system (e.g., /mnt/SDCARD/.system/tg5040/bin/minarch.elf)
        snprintf(dst_path, sizeof(dst_path), "%s/%s", system_dir, files->files[i]);

        // Check if source exists
        if (access(src_path, F_OK) != 0) {
            continue;
        }

        // Copy file
        snprintf(cmd, sizeof(cmd), "cp -f \"%s\" \"%s\"", src_path, dst_path);
        if (system(cmd) != 0) {
            return false;
        }

        // Ensure executable permission
        chmod(dst_path, 0755);
    }

    // Sync filesystem
    sync();

    return true;
}

bool FileOps_restoreOriginals(const char* version, FileList* files) {
    if (!version || strlen(version) == 0 || !files || files->count == 0) return false;

    char cmd[1024];

    // Source directory: bin/{version}-{platform}/original/
    char original_dir[600];
    snprintf(original_dir, sizeof(original_dir), "%s/bin/%s-%s/original", pak_path, version, platform);

    // Check if original directory exists
    if (access(original_dir, F_OK) != 0) {
        return false;
    }

    for (int i = 0; i < files->count; i++) {
        char src_path[600];
        char dst_path[600];

        // Source: just the basename (e.g., minarch.elf)
        const char* basename = get_basename(files->files[i]);
        snprintf(src_path, sizeof(src_path), "%s/%s", original_dir, basename);

        // Destination: full path in system
        snprintf(dst_path, sizeof(dst_path), "%s/%s", system_dir, files->files[i]);

        // Check if source exists
        if (access(src_path, F_OK) != 0) {
            continue;
        }

        // Copy file
        snprintf(cmd, sizeof(cmd), "cp -f \"%s\" \"%s\"", src_path, dst_path);
        if (system(cmd) != 0) {
            return false;
        }

        // Ensure executable permission
        chmod(dst_path, 0755);
    }

    // Sync filesystem
    sync();

    return true;
}

const char* FileOps_getSystemDir(void) {
    return system_dir;
}

NetplayState FileOps_verifyState(const char* version, FileList* files) {
    if (!version || strlen(version) == 0 || !files || files->count == 0) {
        return NETPLAY_STATE_UNKNOWN;
    }

    // Build paths to patched and original directories
    char patched_dir[600];
    char original_dir[600];
    snprintf(patched_dir, sizeof(patched_dir), "%s/bin/%s-%s/patched", pak_path, version, platform);
    snprintf(original_dir, sizeof(original_dir), "%s/bin/%s-%s/original", pak_path, version, platform);

    // Check if version directories exist
    if (access(patched_dir, F_OK) != 0 || access(original_dir, F_OK) != 0) {
        return NETPLAY_STATE_UNKNOWN;
    }

    int patched_matches = 0;
    int original_matches = 0;
    int files_checked = 0;

    for (int i = 0; i < files->count; i++) {
        const char* basename = get_basename(files->files[i]);

        char system_path[600];
        char patched_path[600];
        char original_path[600];

        snprintf(system_path, sizeof(system_path), "%s/%s", system_dir, files->files[i]);
        snprintf(patched_path, sizeof(patched_path), "%s/%s", patched_dir, basename);
        snprintf(original_path, sizeof(original_path), "%s/%s", original_dir, basename);

        // Skip if system file doesn't exist
        if (access(system_path, F_OK) != 0) {
            continue;
        }

        files_checked++;

        // Compare with patched
        if (access(patched_path, F_OK) == 0 && compare_files(system_path, patched_path) == 0) {
            patched_matches++;
        }
        // Compare with original
        else if (access(original_path, F_OK) == 0 && compare_files(system_path, original_path) == 0) {
            original_matches++;
        }
    }

    // Determine state based on matches
    if (files_checked == 0) {
        return NETPLAY_STATE_UNKNOWN;
    }

    // All files match patched
    if (patched_matches == files_checked) {
        return NETPLAY_STATE_ENABLED;
    }
    // All files match original
    if (original_matches == files_checked) {
        return NETPLAY_STATE_DISABLED;
    }

    // Mixed or unknown state
    return NETPLAY_STATE_UNKNOWN;
}
