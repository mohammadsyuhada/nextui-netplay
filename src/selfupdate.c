#include "selfupdate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <zip.h>

#include "include/parson/parson.h"

// Paths
static char pak_path[512] = "";
static char wget_path[512] = "";
static char version_file[512] = "";
static char current_version[32] = "";

// Update status
static SelfUpdateStatus update_status = {0};
static pthread_t update_thread;
static volatile bool update_running = false;
static volatile bool update_cancel = false;

// Forward declarations
static void* check_thread_func(void* arg);
static void* update_thread_func(void* arg);

// Compare semantic versions: returns positive if v1 > v2, negative if v1 < v2, 0 if equal
static int compare_versions(const char* v1, const char* v2) {
    int major1 = 0, minor1 = 0, patch1 = 0;
    int major2 = 0, minor2 = 0, patch2 = 0;

    // Skip 'v' prefix if present
    if (v1[0] == 'v' || v1[0] == 'V') v1++;
    if (v2[0] == 'v' || v2[0] == 'V') v2++;

    sscanf(v1, "%d.%d.%d", &major1, &minor1, &patch1);
    sscanf(v2, "%d.%d.%d", &major2, &minor2, &patch2);

    if (major1 != major2) return major1 - major2;
    if (minor1 != minor2) return minor1 - minor2;
    return patch1 - patch2;
}

// Helper function to create directory path recursively
static int mkpath(const char* path, mode_t mode) {
    char tmp[512];
    char* p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = 0;

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    return mkdir(tmp, mode);
}

// Sync directories: copy all from src to dst, remove orphans in dst
static int sync_directories(const char* src, const char* dst) {
    char cmd[1024];
    DIR* dir;
    struct dirent* entry;

    // First, copy all files from src to dst (overwriting existing)
    snprintf(cmd, sizeof(cmd), "cp -rf \"%s\"/* \"%s\"/ 2>/dev/null", src, dst);
    system(cmd);

    // Now remove orphaned files in dst that don't exist in src
    dir = opendir(dst);
    if (!dir) return -1;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char src_path[600], dst_path[600];
        snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, entry->d_name);

        if (access(src_path, F_OK) != 0) {
            if (entry->d_type == DT_DIR) {
                snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dst_path);
            } else {
                snprintf(cmd, sizeof(cmd), "rm -f \"%s\"", dst_path);
            }
            system(cmd);
        }
        else if (entry->d_type == DT_DIR) {
            sync_directories(src_path, dst_path);
        }
    }

    closedir(dir);
    return 0;
}

// Extract ZIP file using libzip
static int extract_zip(const char* zip_path, const char* dest_dir) {
    int err = 0;
    zip_t* za = zip_open(zip_path, 0, &err);
    if (!za) {
        return -1;
    }

    zip_int64_t num_entries = zip_get_num_entries(za, 0);
    for (zip_int64_t i = 0; i < num_entries; i++) {
        const char* name = zip_get_name(za, i, 0);
        if (!name) continue;

        char full_path[600];
        snprintf(full_path, sizeof(full_path), "%s/%s", dest_dir, name);

        size_t name_len = strlen(name);
        if (name_len > 0 && name[name_len - 1] == '/') {
            mkpath(full_path, 0755);
            continue;
        }

        char* last_slash = strrchr(full_path, '/');
        if (last_slash) {
            *last_slash = '\0';
            mkpath(full_path, 0755);
            *last_slash = '/';
        }

        zip_file_t* zf = zip_fopen_index(za, i, 0);
        if (!zf) continue;

        FILE* out = fopen(full_path, "wb");
        if (!out) {
            zip_fclose(zf);
            continue;
        }

        char buf[8192];
        zip_int64_t bytes_read;
        while ((bytes_read = zip_fread(zf, buf, sizeof(buf))) > 0) {
            fwrite(buf, 1, bytes_read, out);
        }

        fclose(out);
        zip_fclose(zf);

        if (strstr(name, ".elf") || strstr(name, ".sh")) {
            chmod(full_path, 0755);
        }
    }

    zip_close(za);
    return 0;
}

int SelfUpdate_init(const char* path) {
    if (!path) return -1;

    strncpy(pak_path, path, sizeof(pak_path) - 1);

    // Use bundled wget binary (same as music player)
    snprintf(wget_path, sizeof(wget_path), "%s/bin/wget", pak_path);
    snprintf(version_file, sizeof(version_file), "%s/state/app_version.txt", pak_path);

    strncpy(current_version, APP_VERSION_FALLBACK, sizeof(current_version) - 1);
    FILE* f = fopen(version_file, "r");
    if (f) {
        char file_version[32] = "";
        if (fgets(file_version, sizeof(file_version), f)) {
            char* nl = strchr(file_version, '\n');
            if (nl) *nl = '\0';
            if (strlen(file_version) > 0) {
                strncpy(current_version, file_version, sizeof(current_version) - 1);
            }
        }
        fclose(f);
    }

    memset(&update_status, 0, sizeof(update_status));
    strncpy(update_status.current_version, current_version, sizeof(update_status.current_version));

    return 0;
}

void SelfUpdate_cleanup(void) {
    if (update_running) {
        update_cancel = true;
        pthread_join(update_thread, NULL);
    }
}

const char* SelfUpdate_getVersion(void) {
    return current_version;
}

int SelfUpdate_checkForUpdate(void) {
    if (update_running) return -1;

    update_cancel = false;
    update_running = true;

    memset(&update_status, 0, sizeof(update_status));
    update_status.state = SELFUPDATE_STATE_CHECKING;
    strncpy(update_status.current_version, current_version, sizeof(update_status.current_version));
    strcpy(update_status.status_message, "Checking for updates...");

    if (pthread_create(&update_thread, NULL, check_thread_func, NULL) != 0) {
        update_running = false;
        update_status.state = SELFUPDATE_STATE_ERROR;
        strcpy(update_status.error_message, "Failed to start update check");
        return -1;
    }

    return 0;
}

int SelfUpdate_startUpdate(void) {
    if (update_running) return -1;
    if (!update_status.update_available) return -1;

    update_cancel = false;
    update_running = true;

    update_status.state = SELFUPDATE_STATE_DOWNLOADING;
    update_status.progress_percent = 0;
    strcpy(update_status.status_message, "Starting download...");

    if (pthread_create(&update_thread, NULL, update_thread_func, NULL) != 0) {
        update_running = false;
        update_status.state = SELFUPDATE_STATE_ERROR;
        strcpy(update_status.error_message, "Failed to start update");
        return -1;
    }

    return 0;
}

void SelfUpdate_cancelUpdate(void) {
    if (update_running) {
        update_cancel = true;
    }
}

const SelfUpdateStatus* SelfUpdate_getStatus(void) {
    return &update_status;
}

void SelfUpdate_update(void) {
    // Thread handles status updates
}

bool SelfUpdate_isPendingRestart(void) {
    return update_status.state == SELFUPDATE_STATE_COMPLETED;
}

SelfUpdateState SelfUpdate_getState(void) {
    return update_status.state;
}

// Check for update thread
static void* check_thread_func(void* arg) {
    (void)arg;

    int conn = system("ping -c 1 -W 2 8.8.8.8 >/dev/null 2>&1");
    if (conn != 0) {
        conn = system("ping -c 1 -W 2 1.1.1.1 >/dev/null 2>&1");
    }

    if (conn != 0) {
        strcpy(update_status.error_message, "No internet connection");
        update_status.state = SELFUPDATE_STATE_ERROR;
        update_running = false;
        return NULL;
    }

    if (update_cancel) {
        update_status.state = SELFUPDATE_STATE_IDLE;
        update_running = false;
        return NULL;
    }

    update_status.progress_percent = 20;

    char temp_dir[512];
    snprintf(temp_dir, sizeof(temp_dir), "/tmp/netplay_update_%d", getpid());
    mkdir(temp_dir, 0755);

    char latest_file[600];
    snprintf(latest_file, sizeof(latest_file), "%s/latest.json", temp_dir);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "%s -q -U \"NextUI-Netplay\" -O \"%s\" \"https://api.github.com/repos/%s/releases/latest\" 2>/dev/null",
        wget_path, latest_file, APP_GITHUB_REPO);

    if (system(cmd) != 0 || access(latest_file, F_OK) != 0) {
        strcpy(update_status.error_message, "Failed to check GitHub");
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        update_status.state = SELFUPDATE_STATE_ERROR;
        update_running = false;
        return NULL;
    }

    if (update_cancel) {
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        update_status.state = SELFUPDATE_STATE_IDLE;
        update_running = false;
        return NULL;
    }

    update_status.progress_percent = 50;

    char version_cmd[1024];
    snprintf(version_cmd, sizeof(version_cmd),
        "grep -o '\"tag_name\": *\"[^\"]*' \"%s\" | cut -d'\"' -f4",
        latest_file);

    char latest_version[32] = "";
    FILE* pipe = popen(version_cmd, "r");
    if (pipe) {
        if (fgets(latest_version, sizeof(latest_version), pipe)) {
            char* nl = strchr(latest_version, '\n');
            if (nl) *nl = '\0';
        }
        pclose(pipe);
    }

    if (strlen(latest_version) == 0) {
        strcpy(update_status.error_message, "Could not parse version");
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        update_status.state = SELFUPDATE_STATE_ERROR;
        update_running = false;
        return NULL;
    }

    strncpy(update_status.latest_version, latest_version, sizeof(update_status.latest_version));

    update_status.progress_percent = 70;

    int version_cmp = compare_versions(latest_version, current_version);

    if (version_cmp <= 0) {
        update_status.update_available = false;
        strcpy(update_status.status_message, "Already up to date");
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        update_status.state = SELFUPDATE_STATE_IDLE;
        update_running = false;
        return NULL;
    }

    char url_cmd[1024];
    snprintf(url_cmd, sizeof(url_cmd),
        "grep -o '\"browser_download_url\": *\"[^\"]*%s\"' \"%s\" | cut -d'\"' -f4",
        APP_RELEASE_ASSET, latest_file);

    char download_url[512] = "";
    pipe = popen(url_cmd, "r");
    if (pipe) {
        if (fgets(download_url, sizeof(download_url), pipe)) {
            char* nl = strchr(download_url, '\n');
            if (nl) *nl = '\0';
        }
        pclose(pipe);
    }

    if (strlen(download_url) == 0) {
        strcpy(update_status.error_message, "Release package not found");
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        update_status.state = SELFUPDATE_STATE_ERROR;
        update_running = false;
        return NULL;
    }

    strncpy(update_status.download_url, download_url, sizeof(update_status.download_url));

    // Parse release notes (body) from JSON using parson
    // This properly handles all JSON escape sequences
    JSON_Value* json_root = json_parse_file(latest_file);
    if (json_root) {
        JSON_Object* json_obj = json_value_get_object(json_root);
        if (json_obj) {
            const char* body = json_object_get_string(json_obj, "body");
            if (body) {
                strncpy(update_status.release_notes, body, sizeof(update_status.release_notes) - 1);
                update_status.release_notes[sizeof(update_status.release_notes) - 1] = '\0';
            }
        }
        json_value_free(json_root);
    }

    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
    system(cmd);

    update_status.update_available = true;
    snprintf(update_status.status_message, sizeof(update_status.status_message),
        "Update available: %s", latest_version);
    update_status.progress_percent = 100;
    update_status.state = SELFUPDATE_STATE_IDLE;
    update_running = false;

    return NULL;
}

// Update thread - downloads and applies update
static void* update_thread_func(void* arg) {
    (void)arg;

    char cmd[1024];
    char temp_dir[512];
    snprintf(temp_dir, sizeof(temp_dir), "/tmp/netplay_update_%d", getpid());
    mkdir(temp_dir, 0755);

    update_status.state = SELFUPDATE_STATE_DOWNLOADING;
    strcpy(update_status.status_message, "Downloading update...");
    update_status.progress_percent = 5;

    char zip_file[600];
    snprintf(zip_file, sizeof(zip_file), "%s/update.zip", temp_dir);

    snprintf(cmd, sizeof(cmd), "%s -q -U \"NextUI-Netplay\" -O \"%s\" \"%s\" 2>/dev/null",
        wget_path, zip_file, update_status.download_url);

    if (update_cancel) {
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        update_status.state = SELFUPDATE_STATE_IDLE;
        update_running = false;
        return NULL;
    }

    if (system(cmd) != 0 || access(zip_file, F_OK) != 0) {
        strcpy(update_status.error_message, "Download failed");
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        update_status.state = SELFUPDATE_STATE_ERROR;
        update_running = false;
        return NULL;
    }

    update_status.progress_percent = 40;

    if (update_cancel) {
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        update_status.state = SELFUPDATE_STATE_IDLE;
        update_running = false;
        return NULL;
    }

    update_status.state = SELFUPDATE_STATE_EXTRACTING;
    strcpy(update_status.status_message, "Extracting update...");
    update_status.progress_percent = 45;

    char extract_dir[600];
    snprintf(extract_dir, sizeof(extract_dir), "%s/extracted", temp_dir);
    mkdir(extract_dir, 0755);

    if (extract_zip(zip_file, extract_dir) != 0) {
        strcpy(update_status.error_message, "Extraction failed");
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        update_status.state = SELFUPDATE_STATE_ERROR;
        update_running = false;
        return NULL;
    }

    update_status.progress_percent = 60;

    char find_cmd[1024];
    snprintf(find_cmd, sizeof(find_cmd),
        "find \"%s\" -name 'launch.sh' -type f 2>/dev/null | head -1",
        extract_dir);

    char launch_found[600] = "";
    FILE* pipe = popen(find_cmd, "r");
    if (pipe) {
        if (fgets(launch_found, sizeof(launch_found), pipe)) {
            char* nl = strchr(launch_found, '\n');
            if (nl) *nl = '\0';
        }
        pclose(pipe);
    }

    if (strlen(launch_found) == 0) {
        strcpy(update_status.error_message, "Invalid update package");
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        update_status.state = SELFUPDATE_STATE_ERROR;
        update_running = false;
        return NULL;
    }

    char* last_slash = strrchr(launch_found, '/');
    if (last_slash) *last_slash = '\0';
    char update_root[600];
    strncpy(update_root, launch_found, sizeof(update_root));

    update_status.progress_percent = 65;

    if (update_cancel) {
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        update_status.state = SELFUPDATE_STATE_IDLE;
        update_running = false;
        return NULL;
    }

    update_status.state = SELFUPDATE_STATE_APPLYING;
    strcpy(update_status.status_message, "Installing update...");
    update_status.progress_percent = 70;

    if (sync_directories(update_root, pak_path) != 0) {
        strcpy(update_status.error_message, "Failed to install update");
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        update_status.state = SELFUPDATE_STATE_ERROR;
        update_running = false;
        return NULL;
    }

    update_status.progress_percent = 90;

    char binary_path[600], launch_path[600];
    snprintf(binary_path, sizeof(binary_path), "%s/bin/netplay.elf", pak_path);
    chmod(binary_path, 0755);
    snprintf(launch_path, sizeof(launch_path), "%s/launch.sh", pak_path);
    chmod(launch_path, 0755);

    update_status.progress_percent = 95;

    FILE* vf = fopen(version_file, "w");
    if (vf) {
        fprintf(vf, "%s\n", update_status.latest_version);
        fclose(vf);
    }

    sync();

    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
    system(cmd);

    update_status.progress_percent = 100;
    strcpy(update_status.status_message, "Update complete!");
    update_status.state = SELFUPDATE_STATE_COMPLETED;
    update_running = false;

    return NULL;
}
