/**
 * @file task_ctx.h
 * @brief Task context management for VFS
 *
 * Manages per-task VFS state in a multi-threaded FreeRTOS environment.
 * Each FreeRTOS task that uses VFS operations needs a context to store:
 * - Standard I/O file descriptors (stdin, stdout, stderr)
 * - Current working directory
 * - Last errno value for error reporting
 *
 * The context is lazily allocated when a task first performs VFS operations.
 * This allows the VFS to work with multiple concurrent tasks.
 */
#ifndef XV6_VFS_TASK_CTX_H
#define XV6_VFS_TASK_CTX_H

#include "core/types.h"
#include "core/param.h"

/**
 * @brief Per-task VFS context structure
 *
 * Stores VFS state specific to each FreeRTOS task.
 */
typedef struct {
  int stdio_active;      /**< Whether custom stdio FDs are active */
  int in_fd;            /**< Stdin file descriptor (default: 0) */
  int out_fd;           /**< Stdout file descriptor (default: 1) */
  int err_fd;           /**< Stderr file descriptor (default: 2) */
  int last_errno;       /**< Last errno set by VFS operation */
  char cwd[MAXPATH];    /**< Current working directory path */
} xv6_task_ctx_t;

/**
 * @brief Initialize task context subsystem
 *
 * Sets up the task context data structures. Should be called
 * during VFS initialization.
 *
 * @return 0 on success
 */
int vfs_task_ctx_init(void);

/**
 * @brief Get current task's VFS context
 *
 * Returns the context for the calling FreeRTOS task.
 * If no context exists and create is true, allocates a new one.
 *
 * @return Pointer to task context, or NULL if not found and create=0
 */
xv6_task_ctx_t *vfs_task_ctx_get(void);

/**
 * @brief Set errno for current task
 *
 * Stores the error number in the current task's context
 * for later retrieval by the application.
 *
 * @param err Error number (positive value)
 */
void vfs_task_ctx_set_errno(int err);

/**
 * @brief Clear errno for current task
 *
 * Resets the errno to 0 (success) before a new VFS operation.
 */
void vfs_task_ctx_clear_errno(void);

/**
 * @brief Get last errno for current task
 *
 * Returns the errno from the most recent VFS operation
 * in the current task.
 *
 * @return Last errno value, or EIO if none set
 */
int vfs_task_ctx_get_errno(void);

/**
 * @brief Set custom stdio file descriptors
 *
 * Redirects standard I/O for the current task to use
 * the specified file descriptors instead of defaults (0,1,2).
 *
 * @param in_fd Stdin file descriptor
 * @param out_fd Stdout file descriptor
 * @param err_fd Stderr file descriptor
 * @return 0 on success, -1 on error
 */
int vfs_task_ctx_set_stdio(int in_fd, int out_fd, int err_fd);

/**
 * @brief Reset stdio to default file descriptors
 *
 * Restores stdio to use the default file descriptors:
 * 0 (stdin), 1 (stdout), 2 (stderr).
 */
void vfs_task_ctx_reset_stdio(void);

/**
 * @brief Check if default stdout is active
 *
 * Determines whether stdout is using the default FD (1)
 * or has been redirected.
 *
 * @return 1 if using default stdout, 0 if redirected
 */
int vfs_task_ctx_is_default_out(void);

/**
 * @brief Cleanup context for current task
 *
 * Called when a FreeRTOS task exits. Frees the task's
 * context slot and clears its state.
 */
void vfs_task_ctx_cleanup(void);

/**
 * @brief Cleanup context for specific task
 *
 * Frees the context for a task other than the calling one.
 * Used when a task handle is available but the task has exited.
 *
 * @param task_handle FreeRTOS task handle to clean up
 */
void vfs_task_ctx_cleanup_for_handle(void *task_handle);

/**
 * @brief Get current working directory
 *
 * Retrieves the current working directory for the task.
 *
 * @param[out] out Buffer to store the path
 * @param len Buffer size in bytes
 * @return 0 on success, -1 on error
 */
int vfs_task_ctx_getcwd(char *out, int len);

/**
 * @brief Set current working directory
 *
 * Changes the current working directory for the task.
 *
 * @param path New directory path
 * @return 0 on success, -1 on error
 */
int vfs_task_ctx_chdir(const char *path);

#endif
