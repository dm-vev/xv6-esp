/**
 * @file vfs_inode.h
 * @brief Inode operations
 *
 * Provides low-level inode read/write/allocate.
 */
#ifndef XV6_VFS_INODE_H
#define XV6_VFS_INODE_H

#include "core/types.h"
#include "fs/fs.h"

/**
 * @brief Initialize inode subsystem
 * @return 0 on success
 */
int vfs_inode_init(void);

/**
 * @brief Read inode
 * @param inum Inode number
 * @param out Output inode
 * @return 0 on success
 */
int vfs_inode_read(uint32 inum, struct dinode *out);

/**
 * @brief Write inode
 * @param inum Inode number
 * @param in Input inode
 * @return 0 on success
 */
int vfs_inode_write(uint32 inum, const struct dinode *in);

/**
 * @brief Allocate inode
 * @param type Inode type
 * @param out_inum Output inode number
 * @return 0 on success
 */
int vfs_inode_alloc(short type, uint32 *out_inum);

/**
 * @brief Read data from inode
 * @param ip Inode
 * @param off Offset
 * @param dst Destination
 * @param n Size
 * @return Bytes read
 */
int vfs_inode_read_data(const struct dinode *ip, uint32 off, void *dst, uint32 n);

/**
 * @brief Write data to inode
 * @param ip Inode
 * @param off Offset
 * @param src Source
 * @param n Size
 * @return Bytes written
 */
int vfs_inode_write_data(struct dinode *ip, uint32 off, const void *src, uint32 n);

#endif
