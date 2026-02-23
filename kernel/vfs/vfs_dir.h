/**
 * @file vfs_dir.h
 * @brief Directory operations
 *
 * Provides directory manipulation functions for the VFS.
 * Directories are special files that contain entries mapping
 * filenames to inode numbers. Each entry is called a "dirent".
 *
 * Directory operations include:
 * - Creating new directories
 * - Removing directories (must be empty)
 * - Listing directory contents
 * - Adding/removing directory entries
 */
#ifndef XV6_VFS_DIR_H
#define XV6_VFS_DIR_H

#include "core/types.h"

/**
 * @brief Create a new directory
 *
 * Creates a new directory at the specified path. Parent directories
 * must already exist. Creates the "." and ".." entries automatically.
 *
 * @param path Full path where to create the directory
 * @return 0 on success, -1 on error (parent doesn't exist, name conflict, etc.)
 */
int vfs_dir_create(const char *path);

/**
 * @brief Remove a directory
 *
 * Removes a directory from the filesystem. The directory must be empty
 * (only contains "." and ".." entries).
 *
 * @param path Path to the directory to remove
 * @return 0 on success, -1 on error (not empty, doesn't exist, etc.)
 */
int vfs_dir_remove(const char *path);

/**
 * @brief List directory entries
 *
 * Reads a single entry from a directory by index.
 * Use index 0, 1, 2... until the function returns -1 (no more entries).
 *
 * @param path Path to the directory to list
 * @param index Entry index (0-based)
 * @param[out] name_out Buffer to store the entry name
 * @param name_len Size of the name buffer
 * @param[out] type_out Optional output for file type (1=dir, 2=file, 3=device, 4=symlink)
 * @param[out] size_out Optional output for file size
 * @return 0 on success, -1 on error or if index is past the last entry
 */
int vfs_dir_list(const char *path, int index, char *name_out, int name_len, uint16 *type_out, uint32 *size_out);

/**
 * @brief Add an entry to a directory
 *
 * Creates a new directory entry mapping a name to an inode.
 * The inode must already exist and the name must not already exist.
 *
 * @param dir_inum Inode number of the parent directory
 * @param name Name of the new entry
 * @param inum Inode number to associate with the name
 * @return 0 on success, -1 on error
 */
int vfs_dir_add_entry(uint32 dir_inum, const char *name, uint32 inum);

/**
 * @brief Remove an entry from a directory
 *
 * Removes a directory entry by name. Does not affect the target inode
 * (the file itself is not deleted unless this was the last hard link).
 *
 * @param dir_inum Inode number of the parent directory
 * @param name Name of the entry to remove
 * @return 0 on success, -1 on error (entry doesn't exist)
 */
int vfs_dir_remove_entry(uint32 dir_inum, const char *name);

#endif
