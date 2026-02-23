/**
 * @file vfs_dev.h
 * @brief Device file operations
 *
 * Provides /dev/xxx special file handling.
 */
#ifndef XV6_VFS_DEV_H
#define XV6_VFS_DEV_H

#include "core/types.h"

/**
 * @brief Initialize device subsystem
 * @return 0 on success
 */
int vfs_dev_init(void);

/**
 * @brief Check if path is a device
 * @param path File path
 * @return 1 if device, 0 otherwise
 */
int vfs_dev_is_device(const char *path);

/**
 * @brief Read from device
 * @param path Device path
 * @param off Offset
 * @param buf Buffer
 * @param size Size
 * @return Bytes read or error
 */
int vfs_dev_read(const char *path, uint32 off, void *buf, uint32 size);

/**
 * @brief Write to device
 * @param path Device path
 * @param data Data
 * @param size Size
 * @return Bytes written or error
 */
int vfs_dev_write(const char *path, const void *data, uint32 size);

#endif
