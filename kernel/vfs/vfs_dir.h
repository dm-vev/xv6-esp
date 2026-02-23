/**
 * @file vfs_dir.h
 * @brief Directory operations
 *
 * Provides directory manipulation functions.
 */
#ifndef XV6_VFS_DIR_H
#define XV6_VFS_DIR_H

#include "core/types.h"

/**
 * @brief Create directory
 * @param path Directory path
 * @return 0 on success
 */
int vfs_dir_create(const char *path);

/**
 * @brief Remove directory
 * @param path Directory path
 * @return 0 on success
 */
int vfs_dir_remove(const char *path);

/**
 * @brief List directory entries
 * @param path Directory path
 * @param index Entry index
 * @param name_out Output name
 * @param name_len Buffer size
 * @param type_out Output type (optional)
 * @param size_out Output size (optional)
 * @return 0 on success
 */
int vfs_dir_list(const char *path, int index, char *name_out, int name_len, uint16 *type_out, uint32 *size_out);

/**
 * @brief Add entry to directory
 * @param dir_inum Parent directory inode
 * @param name Entry name
 * @param inum Target inode
 * @return 0 on success
 */
int vfs_dir_add_entry(uint32 dir_inum, const char *name, uint32 inum);

/**
 * @brief Remove entry from directory
 * @param dir_inum Parent directory inode
 * @param name Entry name
 * @return 0 on success
 */
int vfs_dir_remove_entry(uint32 dir_inum, const char *name);

#endif
