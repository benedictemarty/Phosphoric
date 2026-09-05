/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file hostfs.h
 * @brief Host filesystem sharing for ORIC emulator
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-01-31
 * @version 0.1.0-alpha
 *
 * Allows the emulated ORIC to access files on the host filesystem.
 * Provides transparent access to host directories from ORIC BASIC/machine code.
 */

#ifndef HOSTFS_H
#define HOSTFS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define HOSTFS_MAX_PATH     256   /**< Maximum path length */
#define HOSTFS_MAX_NAME     16    /**< Maximum ORIC filename length */
#define HOSTFS_MAX_HANDLES  8     /**< Maximum open file handles */

/**
 * @brief File types
 */
typedef enum {
    HOSTFS_TYPE_BASIC,      /**< BASIC program */
    HOSTFS_TYPE_BINARY,     /**< Binary/machine code */
    HOSTFS_TYPE_DATA,       /**< Data file */
    HOSTFS_TYPE_TEXT        /**< Text file */
} hostfs_file_type_t;

/**
 * @brief File handle
 */
typedef struct hostfs_handle_s {
    bool     in_use;            /**< Handle in use flag */
    char     host_path[HOSTFS_MAX_PATH];  /**< Host filesystem path */
    char     oric_name[HOSTFS_MAX_NAME];  /**< ORIC filename */
    FILE*    fp;                /**< Host file pointer */
    bool     writing;           /**< Write mode flag */
    uint32_t position;          /**< Current position */
    uint32_t size;              /**< File size */
} hostfs_handle_t;

/**
 * @brief Host filesystem context
 */
typedef struct hostfs_s {
    char     mount_path[HOSTFS_MAX_PATH];   /**< Mounted directory path */
    bool     mounted;                       /**< Mount status */
    bool     read_only;                     /**< Read-only mode */

    hostfs_handle_t handles[HOSTFS_MAX_HANDLES];  /**< File handles */

    /* Format conversion options */
    bool     auto_convert_basic;    /**< Auto-convert BASIC files */
    bool     auto_convert_binary;   /**< Auto-convert binary files */
} hostfs_t;

/**
 * @brief Initialize host filesystem
 *
 * @param hfs Host filesystem context
 * @return true on success, false on failure
 */
bool hostfs_init(hostfs_t* hfs);

/**
 * @brief Cleanup host filesystem
 *
 * @param hfs Host filesystem context
 */
void hostfs_cleanup(hostfs_t* hfs);

/**
 * @brief Mount host directory
 *
 * @param hfs Host filesystem context
 * @param path Path to host directory
 * @param read_only Mount as read-only
 * @return true on success, false on failure
 */
bool hostfs_mount(hostfs_t* hfs, const char* path, bool read_only);

/**
 * @brief Unmount host directory
 *
 * @param hfs Host filesystem context
 */
void hostfs_unmount(hostfs_t* hfs);

/**
 * @brief Check if host filesystem is mounted
 *
 * @param hfs Host filesystem context
 * @return true if mounted, false otherwise
 */
bool hostfs_is_mounted(const hostfs_t* hfs);

/**
 * @brief Open file from host filesystem
 *
 * @param hfs Host filesystem context
 * @param oric_name ORIC filename (max 16 chars)
 * @param writing true for write, false for read
 * @return File handle (0-7) or -1 on error
 */
int hostfs_open(hostfs_t* hfs, const char* oric_name, bool writing);

/**
 * @brief Close file handle
 *
 * @param hfs Host filesystem context
 * @param handle File handle
 * @return true on success, false on error
 */
bool hostfs_close(hostfs_t* hfs, int handle);

/**
 * @brief Read from file
 *
 * @param hfs Host filesystem context
 * @param handle File handle
 * @param buffer Output buffer
 * @param size Number of bytes to read
 * @return Number of bytes read, or -1 on error
 */
int hostfs_read(hostfs_t* hfs, int handle, uint8_t* buffer, size_t size);

/**
 * @brief Write to file
 *
 * @param hfs Host filesystem context
 * @param handle File handle
 * @param buffer Data buffer
 * @param size Number of bytes to write
 * @return Number of bytes written, or -1 on error
 */
int hostfs_write(hostfs_t* hfs, int handle, const uint8_t* buffer, size_t size);

/**
 * @brief Seek to position in file
 *
 * @param hfs Host filesystem context
 * @param handle File handle
 * @param position Position to seek to
 * @return true on success, false on error
 */
bool hostfs_seek(hostfs_t* hfs, int handle, uint32_t position);

/**
 * @brief Get file size
 *
 * @param hfs Host filesystem context
 * @param handle File handle
 * @return File size in bytes, or 0 on error
 */
uint32_t hostfs_size(hostfs_t* hfs, int handle);

/**
 * @brief List files in mounted directory
 *
 * @param hfs Host filesystem context
 * @param buffer Output buffer for file list
 * @param buffer_size Size of buffer
 * @return Number of files listed
 */
int hostfs_list(hostfs_t* hfs, char* buffer, size_t buffer_size);

/**
 * @brief Delete file
 *
 * @param hfs Host filesystem context
 * @param oric_name ORIC filename
 * @return true on success, false on error
 */
bool hostfs_delete(hostfs_t* hfs, const char* oric_name);

/**
 * @brief Rename file
 *
 * @param hfs Host filesystem context
 * @param old_name Old ORIC filename
 * @param new_name New ORIC filename
 * @return true on success, false on error
 */
bool hostfs_rename(hostfs_t* hfs, const char* old_name, const char* new_name);

/**
 * @brief Convert ORIC filename to host path
 *
 * @param hfs Host filesystem context
 * @param oric_name ORIC filename
 * @param host_path Output host path buffer
 * @param path_size Size of path buffer
 * @return true on success, false on error
 */
bool hostfs_oric_to_host_path(const hostfs_t* hfs, const char* oric_name,
                              char* host_path, size_t path_size);

/**
 * @brief Convert host filename to ORIC name
 *
 * @param host_name Host filename
 * @param oric_name Output ORIC name buffer (must be 17 bytes for null terminator)
 * @return true on success, false on error
 */
bool hostfs_host_to_oric_name(const char* host_name, char* oric_name);

#endif /* HOSTFS_H */
