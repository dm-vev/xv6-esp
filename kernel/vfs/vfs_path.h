/**
 * @file vfs_path.h
 * @brief Path resolution
 *
 * Provides path parsing and resolution utilities for the VFS.
 * Handles:
 * - Absolute vs relative paths
 * - Symbolic link following (or not)
 * - Parent directory extraction
 * - Path component parsing
 *
 * Path resolution follows standard Unix semantics:
 * - Paths starting with '/' are absolute from root
 * - Paths not starting with '/' are relative to current directory
 * - Multiple consecutive slashes are treated as single slash
 * - '.' refers to current directory
 * - '..' refers to parent directory
 */
#ifndef XV6_VFS_PATH_H
#define XV6_VFS_PATH_H

#include "core/types.h"
#include "fs/fs.h"

/**
 * @brief Resolve path to inode
 *
 * Looks up a path and returns the inode number and optionally
 * the inode data. Follows symbolic links to the final target.
 *
 * @param path File path to resolve (absolute or relative to cwd)
 * @param[out] out_inum Output parameter for the inode number
 * @param[out] out_ip Optional output buffer for inode data (can be NULL)
 * @return 0 on success, -1 on error (path not found, permission denied, etc.)
 */
int vfs_path_lookup(const char *path, uint32 *out_inum, struct dinode *out_ip);

/**
 * @brief Resolve path with optional symlink following
 *
 * Resolves a path to its final canonical form. Handles symbolic links
 * by optionally following them to the final target.
 *
 * @param path File path to resolve
 * @param follow Whether to follow symbolic links (1 = follow, 0 = don't follow)
 * @param[out] resolved Buffer to store the resolved path
 * @param len Size of the resolved buffer
 * @return 0 on success, -1 on error
 */
int vfs_path_resolve(const char *path, int follow, char *resolved, int len);

/**
 * @brief Get parent directory and filename
 *
 * Splits a path into its parent directory and final component.
 * For example, "/foo/bar/baz.txt" returns parent "/foo/bar" and name "baz.txt".
 *
 * @param path File path to parse
 * @param[out] parent_out Output parameter for parent inode number
 * @param[out] name_out Buffer to store the final path component
 * @return 0 on success, -1 on error
 */
int vfs_path_parent(const char *path, uint32 *parent_out, char *name_out);

#endif
