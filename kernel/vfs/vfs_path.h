/**
 * @file vfs_path.h
 * @brief Path resolution
 *
 * Provides path parsing and resolution.
 */
#ifndef XV6_VFS_PATH_H
#define XV6_VFS_PATH_H

#include "core/types.h"
#include "fs/fs.h"

/**
 * @brief Resolve path to inode
 * @param path File path
 * @param out_inum Output inode number
 * @param out_ip Output inode (optional)
 * @return 0 on success
 */
int vfs_path_lookup(const char *path, uint32 *out_inum, struct dinode *out_ip);

/**
 * @brief Resolve path with symlink following
 * @param path File path
 * @param follow Follow symlinks
 * @param resolved Resolved path (optional)
 * @param len Buffer size
 * @return 0 on success
 */
int vfs_path_resolve(const char *path, int follow, char *resolved, int len);

/**
 * @brief Get parent directory
 * @param path File path
 * @param parent_out Output parent inode
 * @param name_out Output filename
 * @return 0 on success
 */
int vfs_path_parent(const char *path, uint32 *parent_out, char *name_out);

#endif
