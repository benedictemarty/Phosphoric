/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file vfs.c
 * @brief Virtual Filesystem layer - abstracts tape, disk, and host access
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 0.8.0-alpha
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef enum {
    VFS_TYPE_NONE,
    VFS_TYPE_TAPE,
    VFS_TYPE_DISK,
    VFS_TYPE_HOSTFS
} vfs_type_t;

typedef struct {
    vfs_type_t type;
    void* context;      /* Pointer to tap_file_t, sedoric_disk_t, or hostfs_t */
    bool active;
} vfs_mount_t;

#define VFS_MAX_MOUNTS 4

static vfs_mount_t mounts[VFS_MAX_MOUNTS];

void vfs_init(void) {
    memset(mounts, 0, sizeof(mounts));
}

int vfs_mount(vfs_type_t type, void* context) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].active) {
            mounts[i].type = type;
            mounts[i].context = context;
            mounts[i].active = true;
            return i;
        }
    }
    return -1;
}

void vfs_unmount(int id) {
    if (id >= 0 && id < VFS_MAX_MOUNTS) {
        mounts[id].active = false;
        mounts[id].context = NULL;
    }
}

void* vfs_get_context(int id) {
    if (id >= 0 && id < VFS_MAX_MOUNTS && mounts[id].active)
        return mounts[id].context;
    return NULL;
}

vfs_type_t vfs_get_type(int id) {
    if (id >= 0 && id < VFS_MAX_MOUNTS && mounts[id].active)
        return mounts[id].type;
    return VFS_TYPE_NONE;
}
