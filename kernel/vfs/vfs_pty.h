/**
 * @file vfs_pty.h
 * @brief PTY (pseudo-terminal) management
 *
 * Provides PTY master/slave pair management.
 */
#ifndef XV6_VFS_PTY_H
#define XV6_VFS_PTY_H

#include "core/types.h"

/**
 * @brief Initialize PTY subsystem
 * @return 0 on success
 */
int vfs_pty_init(void);

/**
 * @brief Allocate PTY pair
 * @param master_out Output for master fd
 * @return 0 on success
 */
int vfs_pty_alloc(int *master_out);

/**
 * @brief Get PTY slave name
 * @param master_fd Master fd
 * @param out Buffer for name
 * @param len Buffer size
 * @return 0 on success
 */
int vfs_pty_slave_name(int master_fd, char *out, int len);

/**
 * @brief Close PTY
 * @param id PTY ID
 */
void vfs_pty_close(int id);

#endif
