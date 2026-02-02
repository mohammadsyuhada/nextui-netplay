#include "fileops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

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

// Version string marker to detect and skip during comparison
#define VERSION_MARKER "NextUI ("
#define VERSION_MARKER_LEN 8
#define VERSION_SKIP_LEN 32  // Skip enough bytes to cover "NextUI (YYYY.MM.DD XXXXXXX)"

// Find the offset of version string in a file
// Returns offset if found, -1 if not found
static long find_version_string_offset(FILE* f) {
    fseek(f, 0, SEEK_SET);

    char buf[4096];
    long file_offset = 0;
    size_t bytes_read;

    while ((bytes_read = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i <= bytes_read - VERSION_MARKER_LEN; i++) {
            if (memcmp(buf + i, VERSION_MARKER, VERSION_MARKER_LEN) == 0) {
                return file_offset + i;
            }
        }
        // Handle marker spanning buffer boundary
        file_offset += bytes_read - VERSION_MARKER_LEN + 1;
        fseek(f, file_offset, SEEK_SET);
    }

    return -1;  // Not found
}

// Compare two files byte-by-byte, skipping embedded version strings
// Returns: 0 if identical (ignoring version), 1 if different, -1 on error
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

    // Find version string offsets in both files
    long ver_offset1 = find_version_string_offset(f1);
    long ver_offset2 = find_version_string_offset(f2);

    // If version strings are at different offsets, files have structural differences
    if (ver_offset1 != ver_offset2) {
        // Unless neither has a version string, then compare normally
        if (ver_offset1 != -1 || ver_offset2 != -1) {
            fclose(f1);
            fclose(f2);
            return 1;
        }
    }

    // Compare contents, skipping version string area
    fseek(f1, 0, SEEK_SET);
    fseek(f2, 0, SEEK_SET);

    char buf1[4096];
    char buf2[4096];
    size_t bytes_read;
    long current_offset = 0;

    while ((bytes_read = fread(buf1, 1, sizeof(buf1), f1)) > 0) {
        if (fread(buf2, 1, bytes_read, f2) != bytes_read) {
            fclose(f1);
            fclose(f2);
            return 1;
        }

        // If version string is in this chunk, mask it out before comparison
        if (ver_offset1 >= 0) {
            long chunk_end = current_offset + bytes_read;
            if (ver_offset1 >= current_offset && ver_offset1 < chunk_end) {
                // Version string starts in this chunk
                long mask_start = ver_offset1 - current_offset;
                long mask_end = mask_start + VERSION_SKIP_LEN;
                if (mask_end > (long)bytes_read) mask_end = bytes_read;

                // Zero out the version string area in both buffers
                for (long i = mask_start; i < mask_end; i++) {
                    buf1[i] = 0;
                    buf2[i] = 0;
                }
            }
            // Handle case where version string spans from previous chunk
            else if (ver_offset1 < current_offset && ver_offset1 + VERSION_SKIP_LEN > current_offset) {
                long mask_end = ver_offset1 + VERSION_SKIP_LEN - current_offset;
                if (mask_end > (long)bytes_read) mask_end = bytes_read;

                for (long i = 0; i < mask_end; i++) {
                    buf1[i] = 0;
                    buf2[i] = 0;
                }
            }
        }

        if (memcmp(buf1, buf2, bytes_read) != 0) {
            fclose(f1);
            fclose(f2);
            return 1;  // Different content
        }

        current_offset += bytes_read;
    }

    fclose(f1);
    fclose(f2);
    return 0;  // Identical (ignoring version string)
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

void FileOps_saveInstalledVersion(const char* version_id) {
    if (!version_id) return;

    strncpy(installed_version, version_id, sizeof(installed_version) - 1);

    FILE* f = fopen(version_file, "w");
    if (f) {
        fprintf(f, "%s\n", version_id);
        fclose(f);
    }
}

bool FileOps_parseInstalledVersion(const char* full, char* version_out, int version_size, char* commit_out, int commit_size) {
    if (!full || !version_out || !commit_out) return false;

    version_out[0] = '\0';
    commit_out[0] = '\0';

    // Format: "NextUI-YYYYMMDD-N-COMMITHASH"
    // We need to find the last dash and split there
    const char* last_dash = strrchr(full, '-');
    if (!last_dash || last_dash == full) {
        // No commit hash found, treat entire string as version (legacy format)
        strncpy(version_out, full, version_size - 1);
        version_out[version_size - 1] = '\0';
        return false;  // No commit found
    }

    // Copy commit hash (after last dash)
    strncpy(commit_out, last_dash + 1, commit_size - 1);
    commit_out[commit_size - 1] = '\0';

    // Copy version (before last dash)
    int version_len = last_dash - full;
    if (version_len >= version_size) version_len = version_size - 1;
    strncpy(version_out, full, version_len);
    version_out[version_len] = '\0';

    return true;
}

bool FileOps_isVersionSupported(const char* version, const char* commit) {
    if (!version || strlen(version) == 0) return false;
    if (!commit || strlen(commit) == 0) return false;

    // Check if bin/{version}-{commit}-{platform}/ directory exists
    char version_dir[600];
    snprintf(version_dir, sizeof(version_dir), "%s/bin/%s-%s-%s", pak_path, version, commit, platform);

    return (access(version_dir, F_OK) == 0);
}

bool FileOps_applyPatched(const char* version, const char* commit, FileList* files) {
    if (!version || !commit || !files || files->count == 0) return false;

    char cmd[1024];

    // Source directory: bin/{version}-{commit}-{platform}/patched/
    char patched_dir[600];
    snprintf(patched_dir, sizeof(patched_dir), "%s/bin/%s-%s-%s/patched", pak_path, version, commit, platform);

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

bool FileOps_restoreOriginals(const char* version, const char* commit, FileList* files) {
    if (!version || strlen(version) == 0 || !commit || strlen(commit) == 0 || !files || files->count == 0) return false;

    char cmd[1024];

    // Source directory: bin/{version}-{commit}-{platform}/original/
    char original_dir[600];
    snprintf(original_dir, sizeof(original_dir), "%s/bin/%s-%s-%s/original", pak_path, version, commit, platform);

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

NetplayState FileOps_verifyState(const char* version, const char* commit, FileList* files) {
    if (!version || strlen(version) == 0 || !commit || strlen(commit) == 0 || !files || files->count == 0) {
        return NETPLAY_STATE_UNKNOWN;
    }

    // Build paths to patched and original directories
    char patched_dir[600];
    char original_dir[600];
    snprintf(patched_dir, sizeof(patched_dir), "%s/bin/%s-%s-%s/patched", pak_path, version, commit, platform);
    snprintf(original_dir, sizeof(original_dir), "%s/bin/%s-%s-%s/original", pak_path, version, commit, platform);

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

// Helper to parse version directory name
// Format: "{version}-{commit}-{platform}" e.g., "NextUI-20260130-0-7d98d7f-tg5040"
// Returns true if parsing succeeded
static bool parse_version_dir(const char* dir_name, const char* expected_platform,
                              char* version_out, int version_size,
                              char* commit_out, int commit_size) {
    if (!dir_name || !expected_platform || !version_out || !commit_out) return false;

    // Check if directory ends with expected platform
    int dir_len = strlen(dir_name);
    int plat_len = strlen(expected_platform);

    if (dir_len <= plat_len + 1) return false;

    // Check platform suffix
    const char* suffix = dir_name + dir_len - plat_len;
    if (strcmp(suffix, expected_platform) != 0) return false;

    // Check there's a dash before platform
    if (dir_name[dir_len - plat_len - 1] != '-') return false;

    // Now we have "{version}-{commit}" before the "-{platform}"
    int prefix_len = dir_len - plat_len - 1;
    char prefix[256];
    if (prefix_len >= (int)sizeof(prefix)) return false;
    memcpy(prefix, dir_name, prefix_len);
    prefix[prefix_len] = '\0';

    // Find last dash to split version and commit
    const char* last_dash = strrchr(prefix, '-');
    if (!last_dash || last_dash == prefix) return false;

    // Copy commit (after last dash)
    strncpy(commit_out, last_dash + 1, commit_size - 1);
    commit_out[commit_size - 1] = '\0';

    // Copy version (before last dash)
    int version_len = last_dash - prefix;
    if (version_len >= version_size) version_len = version_size - 1;
    strncpy(version_out, prefix, version_len);
    version_out[version_len] = '\0';

    return true;
}

// Compare function for qsort - sorts version directories in descending order (newest first)
// Version format: "NextUI-YYYYMMDD-N-commit-platform"
static int compare_version_dirs_desc(const void* a, const void* b) {
    const char* dir_a = *(const char**)a;
    const char* dir_b = *(const char**)b;
    // Reverse comparison for descending order (newest first)
    return strcmp(dir_b, dir_a);
}

bool FileOps_findCompatibleVersion(FileList* files, char* version_out, int version_size, char* commit_out, int commit_size) {
    if (!files || files->count == 0 || !version_out || !commit_out) return false;

    version_out[0] = '\0';
    commit_out[0] = '\0';

    // Open bin/ directory
    char bin_dir[600];
    snprintf(bin_dir, sizeof(bin_dir), "%s/bin", pak_path);

    DIR* dir = opendir(bin_dir);
    if (!dir) return false;

    // First pass: collect all matching version directories
    #define MAX_VERSION_DIRS 64
    char* version_dirs[MAX_VERSION_DIRS];
    int dir_count = 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL && dir_count < MAX_VERSION_DIRS) {
        // Skip . and ..
        if (entry->d_name[0] == '.') continue;

        // Try to parse as version directory for our platform
        char ver[64], commit[32];
        if (!parse_version_dir(entry->d_name, platform, ver, sizeof(ver), commit, sizeof(commit))) {
            continue;
        }

        // Store the directory name
        version_dirs[dir_count] = strdup(entry->d_name);
        if (version_dirs[dir_count]) {
            dir_count++;
        }
    }
    closedir(dir);

    if (dir_count == 0) {
        return false;
    }

    // Sort directories in descending order (newest first)
    // Version format "NextUI-YYYYMMDD-N-..." sorts correctly with strcmp
    qsort(version_dirs, dir_count, sizeof(char*), compare_version_dirs_desc);

    // Second pass: check each version for compatibility, starting from newest
    bool found = false;
    for (int d = 0; d < dir_count && !found; d++) {
        char ver[64], commit[32];
        parse_version_dir(version_dirs[d], platform, ver, sizeof(ver), commit, sizeof(commit));

        // Check if this version's original files match current system files
        char original_dir[600];
        snprintf(original_dir, sizeof(original_dir), "%s/%s/original", bin_dir, version_dirs[d]);

        if (access(original_dir, F_OK) != 0) continue;

        // Compare all files
        bool all_match = true;
        int files_checked = 0;

        for (int i = 0; i < files->count && all_match; i++) {
            const char* basename = get_basename(files->files[i]);

            char system_path[600];
            char original_path[600];

            snprintf(system_path, sizeof(system_path), "%s/%s", system_dir, files->files[i]);
            snprintf(original_path, sizeof(original_path), "%s/%s", original_dir, basename);

            // Skip if system file doesn't exist
            if (access(system_path, F_OK) != 0) continue;
            // Skip if original file doesn't exist
            if (access(original_path, F_OK) != 0) continue;

            files_checked++;

            // Compare files
            if (compare_files(system_path, original_path) != 0) {
                all_match = false;
            }
        }

        // If all files matched, we found a compatible version
        if (all_match && files_checked > 0) {
            strncpy(version_out, ver, version_size - 1);
            version_out[version_size - 1] = '\0';
            strncpy(commit_out, commit, commit_size - 1);
            commit_out[commit_size - 1] = '\0';
            found = true;
        }
    }

    // Cleanup allocated strings
    for (int i = 0; i < dir_count; i++) {
        free(version_dirs[i]);
    }

    return found;
}
