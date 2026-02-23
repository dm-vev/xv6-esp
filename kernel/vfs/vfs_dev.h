/**
 * @file vfs_dev.h
 * @brief Device file operations
 *
 * Provides access to special device files in /dev/ directory.
 * These include character devices like console, null, zero, random, etc.
 * Device files are identified by paths starting with /dev/.
 */
#ifndef XV6_VFS_DEV_H
#define XV6_VFS_DEV_H

#include "core/types.h"

/**
 * @brief Initialize device subsystem
 *
 * Sets up the device file subsystem. Currently a no-op but may
 * be used for dynamic device registration in the future.
 *
 * @return 0 on success
 */
int vfs_dev_init(void);

/**
 * @brief Check if a path refers to a device file
 *
 * Determines whether the given path corresponds to a special device file.
 * This includes /dev/null, /dev/zero, /dev/console, /dev/pts/N, etc.
 *
 * @param path File path to check
 * @return 1 if path is a device, 0 otherwise
 */
int vfs_dev_is_device(const char *path);

/**
 * @brief Read from a device file
 *
 * Reads data from a character device. Supported devices:
 * - /dev/null: returns 0 bytes (always empty)
 * - /dev/zero: returns zeros (unlimited null bytes)
 * - /dev/full: returns zeros (simulates full device)
 * - /dev/random, /dev/urandom: returns pseudo-random data
 * - /dev/stdin, /dev/tty: reads from console input
 *
 * @param path Device path
 * @param off Offset (ignored for character devices)
 * @param[out] buf Buffer to store read data
 * @param size Maximum bytes to read
 * @return Number of bytes read, -1 on error (unknown device)
 */
int vfs_dev_read(const char *path, uint32 off, void *buf, uint32 size);

/**
 * @brief Write to a device file
 *
 * Writes data to a character device. Supported devices:
 * - /dev/null: accepts all data, discards it
 * - /dev/zero, /dev/random, /dev/urandom: accepts all data
 * - /dev/console, /dev/tty, /dev/stdout, /dev/stderr: outputs to console
 * - /dev/kmsg: outputs to kernel log
 * - /dev/full: returns error (simulates full device)
 * - /dev/stdin: returns error (read-only)
 *
 * @param path Device path
 * @param data Data to write
 * @param size Number of bytes to write
 * @return Number of bytes written, -1 on error
 */
int vfs_dev_write(const char *path, const void *data, uint32 size);

#endif
