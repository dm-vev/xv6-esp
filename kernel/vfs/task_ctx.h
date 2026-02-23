/**
 * @file task_ctx.h
 * @brief Task context management for VFS
 *
 * Manages per-task VFS state including:
 * - Standard I/O file descriptors
 * - Current working directory
 * - Last errno value
 */
#ifndef XV6_VFS_TASK_CTX_H
#define XV6_VFS_TASK_CTX_H

#include "core/types.h"
#include "core/param.h"

/**
 * @brief Per-task VFS context
 */
typedef struct {
  int stdio_active;      /**< Whether stdio is active */
  int in_fd;            /**< stdin fd */
  int out_fd;           /**< stdout fd */
  int err_fd;           /**< stderr fd */
  int last_errno;       /**< Last errno */
  char cwd[MAXPATH];    /**< Current working directory */
} xv6_task_ctx_t;

/**
 * @brief Initialize task context subsystem
 * @return 0 on success
 */
int vfs_task_ctx_init(void);

/**
 * @brief Get current task's context
 * @return Pointer to task context, or NULL
 */
xv6_task_ctx_t *vfs_task_ctx_get(void);

/**
 * @brief Set errno for current task
 * @param err Error number
 */
void vfs_task_ctx_set_errno(int err);

/**
 * @brief Clear errno for current task
 */
void vfs_task_ctx_clear_errno(void);

/**
 * @brief Get last errno for current task
 * @return Last errno value
 */
int vfs_task_ctx_get_errno(void);

/**
 * @brief Set stdio file descriptors
 * @param in_fd stdin
 * @param out_fd stdout
 * @param err_fd stderr
 * @return 0 on success
 */
int vfs_task_ctx_set_stdio(int in_fd, int out_fd, int err_fd);

/**
 * @brief Reset stdio to defaults (0,1,2)
 */
void vfs_task_ctx_reset_stdio(void);

/**
 * @brief Check if default stdout is active
 * @return 1 if default, 0 otherwise
 */
int vfs_task_ctx_is_default_out(void);

/**
 * @brief Cleanup context for current task
 */
void vfs_task_ctx_cleanup(void);

/**
 * @brief Cleanup context for specific task
 * @param task_handle FreeRTOS task handle
 */
void vfs_task_ctx_cleanup_for_handle(void *task_handle);

/**
 * @brief Get current working directory
 * @param out Buffer for path
 * @param len Buffer size
 * @return 0 on success
 */
int vfs_task_ctx_getcwd(char *out, int len);

/**
 * @brief Set current working directory
 * @param path New directory path
 * @return 0 on success
 */
int vfs_task_ctx_chdir(const char *path);

#endif
