/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file hostfs.c
 * @brief Host filesystem sharing - complete implementation
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 0.8.0-alpha
 */

#include "hostfs/hostfs.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

bool hostfs_init(hostfs_t* hfs) {
    memset(hfs, 0, sizeof(hostfs_t));
    return true;
}

void hostfs_cleanup(hostfs_t* hfs) {
    if (hfs->mounted) hostfs_unmount(hfs);
}

bool hostfs_mount(hostfs_t* hfs, const char* path, bool read_only) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    snprintf(hfs->mount_path, HOSTFS_MAX_PATH, "%s", path);
    hfs->mounted = true;
    hfs->read_only = read_only;
    return true;
}

void hostfs_unmount(hostfs_t* hfs) {
    for (int i = 0; i < HOSTFS_MAX_HANDLES; i++) {
        if (hfs->handles[i].in_use) hostfs_close(hfs, i);
    }
    hfs->mounted = false;
}

bool hostfs_is_mounted(const hostfs_t* hfs) {
    return hfs->mounted;
}

bool hostfs_oric_to_host_path(const hostfs_t* hfs, const char* oric_name,
                              char* host_path, size_t path_size) {
    if (!hfs->mounted) return false;
    snprintf(host_path, path_size, "%s/%s", hfs->mount_path, oric_name);
    return true;
}

bool hostfs_host_to_oric_name(const char* host_name, char* oric_name) {
    const char* base = strrchr(host_name, '/');
    base = base ? base + 1 : host_name;
    size_t len = strlen(base);
    if (len > HOSTFS_MAX_NAME - 1) len = HOSTFS_MAX_NAME - 1;
    for (size_t i = 0; i < len; i++) {
        oric_name[i] = (char)toupper((unsigned char)base[i]);
    }
    oric_name[len] = '\0';
    return true;
}

int hostfs_open(hostfs_t* hfs, const char* oric_name, bool writing) {
    if (!hfs->mounted) return -1;
    if (writing && hfs->read_only) return -1;

    /* Find free handle */
    int handle = -1;
    for (int i = 0; i < HOSTFS_MAX_HANDLES; i++) {
        if (!hfs->handles[i].in_use) { handle = i; break; }
    }
    if (handle < 0) return -1;

    char path[HOSTFS_MAX_PATH];
    hostfs_oric_to_host_path(hfs, oric_name, path, sizeof(path));

    FILE* fp = fopen(path, writing ? "wb" : "rb");
    if (!fp) return -1;

    hostfs_handle_t* h = &hfs->handles[handle];
    h->in_use = true;
    snprintf(h->host_path, HOSTFS_MAX_PATH, "%s", path);
    snprintf(h->oric_name, HOSTFS_MAX_NAME, "%s", oric_name);
    h->fp = fp;
    h->writing = writing;
    h->position = 0;

    if (!writing) {
        fseek(fp, 0, SEEK_END);
        h->size = (uint32_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);
    } else {
        h->size = 0;
    }

    return handle;
}

bool hostfs_close(hostfs_t* hfs, int handle) {
    if (handle < 0 || handle >= HOSTFS_MAX_HANDLES) return false;
    hostfs_handle_t* h = &hfs->handles[handle];
    if (!h->in_use) return false;
    if (h->fp) fclose(h->fp);
    h->in_use = false;
    h->fp = NULL;
    return true;
}

int hostfs_read(hostfs_t* hfs, int handle, uint8_t* buffer, size_t size) {
    if (handle < 0 || handle >= HOSTFS_MAX_HANDLES) return -1;
    hostfs_handle_t* h = &hfs->handles[handle];
    if (!h->in_use || !h->fp || h->writing) return -1;

    size_t rd = fread(buffer, 1, size, h->fp);
    h->position += (uint32_t)rd;
    return (int)rd;
}

int hostfs_write(hostfs_t* hfs, int handle, const uint8_t* buffer, size_t size) {
    if (handle < 0 || handle >= HOSTFS_MAX_HANDLES) return -1;
    hostfs_handle_t* h = &hfs->handles[handle];
    if (!h->in_use || !h->fp || !h->writing) return -1;

    size_t wr = fwrite(buffer, 1, size, h->fp);
    h->position += (uint32_t)wr;
    if (h->position > h->size) h->size = h->position;
    return (int)wr;
}

bool hostfs_seek(hostfs_t* hfs, int handle, uint32_t position) {
    if (handle < 0 || handle >= HOSTFS_MAX_HANDLES) return false;
    hostfs_handle_t* h = &hfs->handles[handle];
    if (!h->in_use || !h->fp) return false;
    if (fseek(h->fp, (long)position, SEEK_SET) != 0) return false;
    h->position = position;
    return true;
}

uint32_t hostfs_size(hostfs_t* hfs, int handle) {
    if (handle < 0 || handle >= HOSTFS_MAX_HANDLES) return 0;
    if (hfs->handles[handle].in_use) return hfs->handles[handle].size;
    return 0;
}

int hostfs_list(hostfs_t* hfs, char* buffer, size_t buffer_size) {
    if (!hfs->mounted) return 0;
    DIR* dir = opendir(hfs->mount_path);
    if (!dir) return 0;

    int count = 0;
    size_t pos = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        size_t nlen = strlen(entry->d_name);
        if (pos + nlen + 2 >= buffer_size) break;
        memcpy(buffer + pos, entry->d_name, nlen);
        pos += nlen;
        buffer[pos++] = '\n';
        count++;
    }
    if (pos > 0) buffer[pos - 1] = '\0';
    else if (buffer_size > 0) buffer[0] = '\0';
    closedir(dir);
    return count;
}

bool hostfs_delete(hostfs_t* hfs, const char* oric_name) {
    if (!hfs->mounted || hfs->read_only) return false;
    char path[HOSTFS_MAX_PATH];
    hostfs_oric_to_host_path(hfs, oric_name, path, sizeof(path));
    return unlink(path) == 0;
}

bool hostfs_rename(hostfs_t* hfs, const char* old_name, const char* new_name) {
    if (!hfs->mounted || hfs->read_only) return false;
    char old_path[HOSTFS_MAX_PATH], new_path[HOSTFS_MAX_PATH];
    hostfs_oric_to_host_path(hfs, old_name, old_path, sizeof(old_path));
    hostfs_oric_to_host_path(hfs, new_name, new_path, sizeof(new_path));
    return rename(old_path, new_path) == 0;
}
