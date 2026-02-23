/**
 * @file vfs_pty.h
 * @brief PTY (pseudo-terminal) management
 *
 * Provides PTY master/slave pair management for terminal emulation.
 * PTYs allow communication between a master process and a slave process
 * as if they were connected by a physical terminal.
 */
#ifndef XV6_VFS_PTY_H
#define XV6_VFS_PTY_H

#include "core/types.h"

/**
 * @brief Initialize PTY subsystem
 *
 * Sets up the PTY data structures and prepares the subsystem for use.
 * Must be called before any PTY operations.
 *
 * @return 0 on success, -1 on failure
 */
int vfs_pty_init(void);

/**
 * @brief Allocate a new PTY pair
 *
 * Creates a new pseudo-terminal pair, returning the master end file descriptor.
 * The slave end is created implicitly and can be accessed via ptsname().
 *
 * @param[out] master_out Output pointer to store the master PTY id
 * @return 0 on success, -1 on failure (no free PTY slots)
 */
int vfs_pty_alloc(int *master_out);

/**
 * @brief Get the slave PTY device name for a given master
 *
 * Returns the path to the slave device corresponding to the master,
 * e.g., "/dev/pts/0" for master with id 0.
 *
 * @param master_fd Master PTY file descriptor
 * @param[out] out Buffer to store the slave device path
 * @param len Buffer size in bytes
 * @return 0 on success, -1 on failure (invalid master_fd or buffer too small)
 */
int vfs_pty_slave_name(int master_fd, char *out, int len);

/**
 * @brief Close a PTY and update reference counts
 *
 * Decrements the reference count for the PTY. If both master and slave
 * are closed and no data remains in the buffers, the PTY is freed.
 *
 * @param id PTY identifier (returned by vfs_pty_alloc)
 */
void vfs_pty_close(int id);

/**
 * @brief Read data from PTY
 *
 * Reads data from either the master or slave end of the PTY.
 * For master: reads data written to slave (s2m queue)
 * For slave: reads data written to master (m2s queue)
 *
 * @param id PTY identifier
 * @param is_master 1 for master end, 0 for slave end
 * @param[out] buf Buffer to store read data
 * @param size Maximum number of bytes to read
 * @return Number of bytes read, 0 if no data available, -1 on error
 */
int vfs_pty_read(int id, int is_master, void *buf, uint32 size);

/**
 * @brief Write data to PTY
 *
 * Writes data to either the master or slave end of the PTY.
 * For master: writes to m2s queue (slave will read)
 * For slave: writes to s2m queue (master will read)
 *
 * @param id PTY identifier
 * @param is_master 1 for master end, 0 for slave end
 * @param data Data to write
 * @param size Number of bytes to write
 * @return Number of bytes written, -1 on error
 */
int vfs_pty_write(int id, int is_master, const void *buf, uint32 size);

#endif
