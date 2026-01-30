#include "download.h"
#include "netplay_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <zip.h>

// Paths
static char pak_path[512] = "";
static char wget_path[512] = "";
static char temp_dir[512] = "";
static char dest_dir[512] = "";
static char download_version[64] = "";
static char download_platform[32] = "";

// Download status
static DownloadStatus download_status = {0};
static pthread_t download_thread;
static volatile bool download_running = false;
static volatile bool download_cancel = false;

// Forward declarations
static void* download_thread_func(void* arg);

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

        // Check if it's a directory
        size_t name_len = strlen(name);
        if (name_len > 0 && name[name_len - 1] == '/') {
            mkpath(full_path, 0755);
            continue;
        }

        // Create parent directory if needed
        char* last_slash = strrchr(full_path, '/');
        if (last_slash) {
            *last_slash = '\0';
            mkpath(full_path, 0755);
            *last_slash = '/';
        }

        // Extract file
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

        // Preserve executable permission for .elf and .so files
        if (strstr(name, ".elf") || strstr(name, ".so")) {
            chmod(full_path, 0755);
        }
    }

    zip_close(za);
    return 0;
}

void Download_init(const char* path) {
    if (!path) return;

    strncpy(pak_path, path, sizeof(pak_path) - 1);
    snprintf(wget_path, sizeof(wget_path), "/mnt/SDCARD/.system/bin/wget");

    memset(&download_status, 0, sizeof(download_status));
}

void Download_cleanup(void) {
    if (download_running) {
        download_cancel = true;
        pthread_join(download_thread, NULL);
    }
}

bool Download_checkInternet(void) {
    int conn = system("ping -c 1 -W 2 8.8.8.8 >/dev/null 2>&1");
    if (conn != 0) {
        conn = system("ping -c 1 -W 2 1.1.1.1 >/dev/null 2>&1");
    }
    return (conn == 0);
}

bool Download_isVersionSupported(const char* version, const char* platform) {
    char* url = Download_getAssetUrl(version, platform);
    if (url) {
        free(url);
        return true;
    }
    return false;
}

char* Download_getAssetUrl(const char* version, const char* platform) {
    // Create temp directory
    char temp[512];
    snprintf(temp, sizeof(temp), "/tmp/netplay_check_%d", getpid());
    mkdir(temp, 0755);

    // Fetch release info from GitHub API
    char latest_file[600];
    snprintf(latest_file, sizeof(latest_file), "%s/release.json", temp);

    // Try to get release by tag (version)
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "%s -q -U \"NextUI-Netplay\" -O \"%s\" \"https://api.github.com/repos/%s/releases/tags/%s\" 2>/dev/null",
        wget_path, latest_file, NETPLAY_GITHUB_REPO, version);

    if (system(cmd) != 0 || access(latest_file, F_OK) != 0) {
        // Clean up
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp);
        system(cmd);
        return NULL;
    }

    // Parse download URL for the platform-specific asset
    // Asset naming: {version}-{platform}.zip
    char asset_pattern[256];
    snprintf(asset_pattern, sizeof(asset_pattern), "%s-%s.zip", version, platform);

    char url_cmd[1024];
    snprintf(url_cmd, sizeof(url_cmd),
        "grep -o '\"browser_download_url\": *\"[^\"]*%s\"' \"%s\" | cut -d'\"' -f4",
        asset_pattern, latest_file);

    char download_url[512] = "";
    FILE* pipe = popen(url_cmd, "r");
    if (pipe) {
        if (fgets(download_url, sizeof(download_url), pipe)) {
            char* nl = strchr(download_url, '\n');
            if (nl) *nl = '\0';
        }
        pclose(pipe);
    }

    // Clean up
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp);
    system(cmd);

    if (strlen(download_url) > 0) {
        return strdup(download_url);
    }
    return NULL;
}

int Download_start(const char* version, const char* platform, const char* destination) {
    if (download_running) return -1;

    strncpy(download_version, version, sizeof(download_version) - 1);
    strncpy(download_platform, platform, sizeof(download_platform) - 1);
    strncpy(dest_dir, destination, sizeof(dest_dir) - 1);

    snprintf(temp_dir, sizeof(temp_dir), "/tmp/netplay_download_%d", getpid());
    mkdir(temp_dir, 0755);

    download_cancel = false;
    download_running = true;

    download_status.state = DOWNLOAD_STATE_CHECKING;
    download_status.progress_percent = 0;
    strcpy(download_status.status_message, "Checking for netplay files...");
    download_status.error_message[0] = '\0';

    if (pthread_create(&download_thread, NULL, download_thread_func, NULL) != 0) {
        download_running = false;
        download_status.state = DOWNLOAD_STATE_ERROR;
        strcpy(download_status.error_message, "Failed to start download");
        return -1;
    }

    return 0;
}

void Download_cancel(void) {
    if (download_running) {
        download_cancel = true;
    }
}

const DownloadStatus* Download_getStatus(void) {
    return &download_status;
}

void Download_update(void) {
    // Thread handles status updates
}

bool Download_isRunning(void) {
    return download_running;
}

// Download thread function
static void* download_thread_func(void* arg) {
    (void)arg;
    char cmd[1024];

    // Get download URL
    char* download_url = Download_getAssetUrl(download_version, download_platform);
    if (!download_url) {
        strcpy(download_status.error_message, "Version not supported");
        download_status.state = DOWNLOAD_STATE_ERROR;
        download_running = false;
        return NULL;
    }

    if (download_cancel) {
        free(download_url);
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        download_status.state = DOWNLOAD_STATE_IDLE;
        download_running = false;
        return NULL;
    }

    // Download the ZIP file
    download_status.state = DOWNLOAD_STATE_DOWNLOADING;
    strcpy(download_status.status_message, "Downloading netplay files...");
    download_status.progress_percent = 10;

    char zip_file[600];
    snprintf(zip_file, sizeof(zip_file), "%s/netplay.zip", temp_dir);

    snprintf(cmd, sizeof(cmd), "%s -q -U \"NextUI-Netplay\" -O \"%s\" \"%s\" 2>/dev/null",
        wget_path, zip_file, download_url);
    free(download_url);

    if (download_cancel) {
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        download_status.state = DOWNLOAD_STATE_IDLE;
        download_running = false;
        return NULL;
    }

    if (system(cmd) != 0 || access(zip_file, F_OK) != 0) {
        strcpy(download_status.error_message, "Download failed");
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        download_status.state = DOWNLOAD_STATE_ERROR;
        download_running = false;
        return NULL;
    }

    download_status.progress_percent = 50;

    if (download_cancel) {
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        download_status.state = DOWNLOAD_STATE_IDLE;
        download_running = false;
        return NULL;
    }

    // Extract the ZIP file
    download_status.state = DOWNLOAD_STATE_EXTRACTING;
    strcpy(download_status.status_message, "Extracting files...");
    download_status.progress_percent = 60;

    char extract_dir[600];
    snprintf(extract_dir, sizeof(extract_dir), "%s/extracted", temp_dir);
    mkdir(extract_dir, 0755);

    if (extract_zip(zip_file, extract_dir) != 0) {
        strcpy(download_status.error_message, "Extraction failed");
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        download_status.state = DOWNLOAD_STATE_ERROR;
        download_running = false;
        return NULL;
    }

    download_status.progress_percent = 80;

    // Move files to destination
    snprintf(cmd, sizeof(cmd), "cp -rf \"%s\"/* \"%s\"/ 2>/dev/null", extract_dir, dest_dir);
    system(cmd);

    download_status.progress_percent = 95;

    // Clean up temp directory
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
    system(cmd);

    download_status.progress_percent = 100;
    strcpy(download_status.status_message, "Download complete");
    download_status.state = DOWNLOAD_STATE_COMPLETED;
    download_running = false;

    return NULL;
}
