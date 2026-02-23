/**
 * @file vfs_inode.h
 * @brief Inode operations
 *
 * Provides low-level inode (index node) operations for the xv6 filesystem.
 * Inodes are the fundamental metadata structures representing files and directories.
 * Each inode stores:
 * - File type (regular file, directory, symbolic link, device)
 * - File size
 * - Block pointers for file data
 * - Reference count (number of hard links)
 * - Owner UID/GID and permissions
 */
#ifndef XV6_VFS_INODE_H
#define XV6_VFS_INODE_H

#include "core/types.h"
#include "fs/fs.h"

/**
 * @brief Initialize inode subsystem
 *
 * Sets up the inode cache and prepares for filesystem operations.
 * Should be called during VFS initialization.
 *
 * @return 0 on success
 */
int vfs_inode_init(void);

/**
 * @brief Read inode from disk
 *
 * Loads the inode with the given number from the filesystem.
 *
 * @param inum Inode number to read
 * @param[out] out Output buffer for the inode data
 * @return 0 on success, -1 on error (invalid inode number)
 */
int vfs_inode_read(uint32 inum, struct dinode *out);

/**
 * @brief Write inode to disk
 *
 * Stores the inode data back to the filesystem at the given location.
 *
 * @param inum Inode number to write
 * @param in Input inode data
 * @return 0 on success, -1 on error
 */
int vfs_inode_write(uint32 inum, const struct dinode *in);

/**
 * @brief Allocate a new inode
 *
 * Creates a new inode of the specified type in the filesystem.
 * The inode is allocated from the free inode list.
 *
 * @param type Inode type (T_FILE, T_DIR, T_DEV, T_SYMLINK from fs.h)
 * @param[out] out_inum Output parameter for the new inode number
 * @return 0 on success, -1 on error (no free inodes)
 */
int vfs_inode_alloc(short type, uint32 *out_inum);

/**
 * @brief Read data from an inode
 *
 * Reads data from the file represented by the inode.
 * Handles direct and indirect block addressing.
 *
 * @param ip Pointer to the inode
 * @param off Offset within the file to start reading
 * @param[out] dst Destination buffer for read data
 * @param n Maximum number of bytes to read
 * @return Number of bytes read, 0 on EOF, -1 on error
 */
int vfs_inode_read_data(const struct dinode *ip, uint32 off, void *dst, uint32 n);

/**
 * @brief Write data to an inode
 *
 * Writes data to the file represented by the inode.
 * Allocates new blocks as needed when extending the file.
 *
 * @param ip Pointer to the inode (modified in place)
 * @param off Offset within the file to start writing
 * @param src Source data to write
 * @param n Number of bytes to write
 * @return Number of bytes written, -1 on error
 */
int vfs_inode_write_data(struct dinode *ip, uint32 off, const void *src, uint32 n);

#endif
