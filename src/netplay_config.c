#include "netplay_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Load file list from configuration
FileList* Config_loadFiles(const char* conf_path) {
    FILE* f = fopen(conf_path, "r");
    if (!f) return NULL;

    FileList* list = calloc(1, sizeof(FileList));
    if (!list) {
        fclose(f);
        return NULL;
    }

    // First pass: count lines
    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        // Skip empty lines and comments
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '#') continue;
        count++;
    }

    if (count == 0) {
        fclose(f);
        free(list);
        return NULL;
    }

    // Allocate file array
    list->files = calloc(count, sizeof(char*));
    if (!list->files) {
        fclose(f);
        free(list);
        return NULL;
    }
    list->count = count;

    // Second pass: read files
    rewind(f);
    int idx = 0;
    while (fgets(line, sizeof(line), f) && idx < count) {
        // Skip empty lines and comments
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '#') continue;

        // Remove trailing newline
        char* nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        char* cr = strchr(p, '\r');
        if (cr) *cr = '\0';

        // Trim trailing whitespace
        int len = strlen(p);
        while (len > 0 && (p[len-1] == ' ' || p[len-1] == '\t')) {
            p[--len] = '\0';
        }

        if (len > 0) {
            list->files[idx] = strdup(p);
            idx++;
        }
    }

    list->count = idx;  // Actual count after parsing
    fclose(f);
    return list;
}

// Free file list
void Config_freeFiles(FileList* list) {
    if (!list) return;
    if (list->files) {
        for (int i = 0; i < list->count; i++) {
            free(list->files[i]);
        }
        free(list->files);
    }
    free(list);
}

// Get NextUI version from system
bool Config_getSystemVersion(char* buffer, int buffer_size) {
    if (!buffer || buffer_size < 1) return false;
    buffer[0] = '\0';

    FILE* f = fopen(VERSION_FILE_PATH, "r");
    if (!f) return false;

    if (fgets(buffer, buffer_size, f)) {
        // Remove trailing newline
        char* nl = strchr(buffer, '\n');
        if (nl) *nl = '\0';
        char* cr = strchr(buffer, '\r');
        if (cr) *cr = '\0';
    }

    fclose(f);
    return (strlen(buffer) > 0);
}

// Get NextUI commit hash from system (line 2 of version.txt)
bool Config_getSystemCommit(char* buffer, int buffer_size) {
    if (!buffer || buffer_size < 1) return false;
    buffer[0] = '\0';

    FILE* f = fopen(VERSION_FILE_PATH, "r");
    if (!f) return false;

    char line[128];
    // Skip first line (version)
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return false;
    }

    // Read second line (commit hash)
    if (fgets(buffer, buffer_size, f)) {
        // Remove trailing newline
        char* nl = strchr(buffer, '\n');
        if (nl) *nl = '\0';
        char* cr = strchr(buffer, '\r');
        if (cr) *cr = '\0';
    }

    fclose(f);
    return (strlen(buffer) > 0);
}
