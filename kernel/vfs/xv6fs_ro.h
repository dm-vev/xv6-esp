/**
 * @file xv6fs_ro.h
 * @brief Read-only xv6 virtual file system interface
 *
 * This header defines the VFS layer for xv6 running on ESP32. It provides:
 * - Flash-based read-only filesystem
 * - File descriptor-based API for user programs
 * - PTY and pipe support for shell functionality
 * - Task context management for FreeRTOS integration
 *
 * Capacity limits:
 * - XV6_TASK_CTX_CAP: Maximum concurrent task contexts (64)
 * - XV6_FD_CAP: Maximum open file descriptors (128)
 * - XV6_PTY_CAP: Maximum PTY pairs (8)
 * - XV6_PIPE_CAP: Maximum pipes (64)
 */
#ifndef XV6_XV6FS_RO_H
#define XV6_XV6FS_RO_H

#include "core/types.h"

/**
 * @brief Maximum number of concurrent task contexts
 *
 * Each FreeRTOS task that uses xv6 file operations needs a context
 * to store stdio fds, current working directory, and errno.
 */
#define XV6_TASK_CTX_CAP 64

/**
 * @brief Maximum number of open file descriptors per process
 */
#define XV6_FD_CAP 128

/**
 * @brief Maximum number of PTY pairs
 */
#define XV6_PTY_CAP 8

/**
 * @brief Maximum number of pipes
 */
#define XV6_PIPE_CAP 64

/**
 * @brief Initialize the read-only VFS layer
 * @return 0 on success, -1 on failure
 *
 * Initializes the VFS subsystem including file descriptor table,
 * PTY management, pipe management, and task context system.
 * Must be called before any other VFS operations.
 */
int xv6fs_ro_init(void);

/**
 * @brief Load a filesystem image from flash
 * @param image Pointer to filesystem image in flash
 * @param image_size Size of image in bytes
 * @return 0 on success, -1 on failure
 *
 * Parses the filesystem image superblock and prepares the VFS
 * for file operations. The image should be a valid xv6 filesystem.
 */
int xv6fs_ro_flash_image(const uint8 *image, uint32 image_size);

/**
 * @brief List files in root directory
 * @param index Entry index to retrieve
 * @param name_out Buffer for file name
 * @param name_out_len Size of name buffer
 * @param size_out Pointer for file size (optional)
 * @return 0 on success, -1 on error/EOF
 *
 * Iterates through root directory entries. Use index 0, 1, 2...
 * until function returns -1 (EOF).
 */
int xv6fs_ro_list(int index, char *name_out, int name_out_len, uint32 *size_out);

/**
 * @brief Read file into allocated memory
 * @param name File name in root
 * @param out_data Pointer to store allocated buffer
 * @param out_size Pointer to store file size
 * @return 0 on success, -1 on failure
 *
 * Loads entire file into heap-allocated buffer. Caller must free
 * the buffer when done.
 */
int xv6fs_ro_read_file_alloc(const char *name, void **out_data, uint32 *out_size);

/**
 * @brief Read file by path into allocated memory
 * @param path Full path to file
 * @param out_data Pointer to store allocated buffer
 * @param out_size Pointer to store file size
 * @return 0 on success, -1 on failure
 *
 * Like xv6fs_ro_read_file_alloc but accepts full path.
 */
int xv6fs_read_file_alloc_path(const char *path, void **out_data, uint32 *out_size);

/**
 * @brief List directory contents by path
 * @param path Directory path
 * @param index Entry index
 * @param name_out Buffer for entry name
 * @param name_out_len Buffer size
 * @param type_out Pointer for file type
 * @param size_out Pointer for file size
 * @return 0 on success, -1 on error/EOF
 *
 * Lists contents of specified directory. Valid type values:
 * - 1: directory
 * - 2: regular file
 * - 3: device
 */
int xv6fs_list_path(const char *path, int index, char *name_out, int name_out_len, uint16 *type_out,
                    uint32 *size_out);

/**
 * @brief Write data to file (only for /dev/xxx special files)
 * @param path File path
 * @param data Data to write
 * @param size Size of data
 * @return Bytes written on success, -1 on failure
 *
 * Note: Only supports writing to special device files.
 * Regular files are read-only from flash.
 */
int xv6fs_write_file_path(const char *path, const void *data, uint32 size);

/**
 * @brief Create directory
 * @param path Directory path to create
 * @return 0 on success, -1 on failure
 *
 * Creates all components of the path that don't exist.
 */
int xv6fs_mkdir_path(const char *path);

/**
 * @brief Remove file
 * @param path File path to remove
 * @return 0 on success, -1 on failure
 *
 * Only works on files created at runtime (not flash files).
 */
int xv6fs_unlink_path(const char *path);

/**
 * @brief Remove directory
 * @param path Directory path to remove
 * @return 0 on success, -1 on failure
 *
 * Directory must be empty.
 */
int xv6fs_rmdir_path(const char *path);

/**
 * @brief Rename file or directory
 * @param oldpath Current path
 * @param newpath New path
 * @return 0 on success, -1 on failure
 */
int xv6fs_rename_path(const char *oldpath, const char *newpath);

/**
 * @brief Open file
 * @param path File path
 * @param flags Open flags (XV6_O_RDONLY, XV6_O_WRONLY, etc.)
 * @return File descriptor on success, -1 on failure
 *
 * Opens a file and returns a file descriptor for subsequent
 * read/write/seek/close operations.
 */
int xv6_open(const char *path, int flags);

/**
 * @brief Duplicate file descriptor
 * @param fd File descriptor to duplicate
 * @return New file descriptor on success, -1 on failure
 */
int xv6_dup(int fd);

/**
 * @brief Read from file descriptor
 * @param fd File descriptor
 * @param buf Buffer for data
 * @param size Maximum bytes to read
 * @return Bytes read on success, 0 on EOF, -1 on failure
 */
int xv6_read(int fd, void *buf, uint32 size);

/**
 * @brief Write to file descriptor
 * @param fd File descriptor
 * @param buf Data to write
 * @param size Bytes to write
 * @return Bytes written on success, -1 on failure
 */
int xv6_write(int fd, const void *buf, uint32 size);

/**
 * @brief Close file descriptor
 * @param fd File descriptor to close
 * @return 0 on success, -1 on failure
 */
int xv6_close(int fd);

/**
 * @brief Change file offset
 * @param fd File descriptor
 * @param offset Offset relative to whence
 * @param whence SEEK_SET (0), SEEK_CUR (1), or SEEK_END (2)
 * @return New offset on success, -1 on failure
 */
int xv6_lseek(int fd, int offset, int whence);

/**
 * @brief Set file status flags
 * @param fd File descriptor
 * @param status_flags Flags (currently only XV6_O_APPEND supported)
 * @return 0 on success, -1 on failure
 */
int xv6_set_status_flags(int fd, int status_flags);

/**
 * @brief Change current working directory
 * @param path New directory path
 * @return 0 on success, -1 on failure
 */
int xv6_chdir(const char *path);

/**
 * @brief Get current working directory
 * @param out_path Buffer for path
 * @param out_len Buffer size
 * @return 0 on success, -1 on failure
 */
int xv6_getcwd(char *out_path, int out_len);

/**
 * @brief Get PTY slave device name
 * @param master_fd PTY master file descriptor
 * @param out_path Buffer for slave path
 * @param out_len Buffer size
 * @return 0 on success, -1 on failure
 *
 * Returns path like "/dev/pts/0" for the slave device
 * corresponding to the given master.
 */
int xv6_ptsname(int master_fd, char *out_path, int out_len);

/**
 * @brief Create pipe
 * @param out_read_fd Pointer for read end fd
 * @param out_write_fd Pointer for write end fd
 * @return 0 on success, -1 on failure
 *
 * Creates a unidirectional pipe. Data written to write end
 * can be read from read end.
 */
int xv6_pipe(int *out_read_fd, int *out_write_fd);

/**
 * @brief Check file accessibility
 * @param path File path
 * @param mode Access mode (R_OK, W_OK, X_OK)
 * @return 0 if accessible, -1 otherwise
 */
int xv6_access(const char *path, int mode);

/**
 * @brief Change file mode
 * @param path File path
 * @param mode New mode (permissions)
 * @return 0 on success, -1 on failure
 *
 * Note: Only works on runtime files, not flash files.
 */
int xv6_chmod(const char *path, int mode);

/**
 * @brief File metadata structure
 *
 * Contains basic file metadata returned by stat/fstat.
 */
typedef struct {
  uint32 ino;     /**< Inode number */
  uint32 size;    /**< File size in bytes */
  uint16 type;    /**< File type (1=dir, 2=file, 3=device) */
  uint16 nlink;   /**< Number of hard links */
} xv6_kstat_t;

/**
 * @brief Get file status by path
 * @param path File path
 * @param st Buffer for metadata
 * @return 0 on success, -1 on failure
 */
int xv6_stat_path(const char *path, xv6_kstat_t *st);

/**
 * @brief Get file status by descriptor
 * @param fd File descriptor
 * @param st Buffer for metadata
 * @return 0 on success, -1 on failure
 */
int xv6_fstat(int fd, xv6_kstat_t *st);

/**
 * @brief Get last errno for current task
 * @return Last errno value
 *
 * Returns the last errno set by VFS operations for the
 * current FreeRTOS task.
 */
int xv6_last_errno(void);

/**
 * @brief Set stdio file descriptors
 * @param in_fd Stdin fd
 * @param out_fd Stdout fd
 * @param err_fd Stderr fd
 * @return 0 on success, -1 on failure
 *
 * Redirects standard input/output/error for current task.
 */
int xv6_stdio_set_fds(int in_fd, int out_fd, int err_fd);

/**
 * @brief Reset stdio to defaults (0, 1, 2)
 *
 * Resets the stdio file descriptors for current task to
 * the default values (0=stdin, 1=stdout, 2=stderr).
 */
void xv6_stdio_reset_fds(void);

/**
 * @brief Check if default stdout is active
 * @return 1 if default stdout, 0 otherwise
 */
int xv6_stdio_is_default_out(void);

/**
 * @brief Cleanup task context
 *
 * Called when a FreeRTOS task exits. Closes all open
 * file descriptors and frees resources associated with
 * the task's VFS context.
 */
void xv6_task_ctx_cleanup(void);

/**
 * @brief Cleanup context for specific task
 * @param task_handle FreeRTOS task handle
 *
 * Cleanup VFS context for a specific task (not the current one).
 */
void xv6_task_ctx_cleanup_for_handle(void *task_handle);

/**
 * @brief Reset entire VFS state
 *
 * Closes all file descriptors, resets task contexts,
 * and reinitializes the VFS subsystem. Used during
 * error recovery or system reset.
 */
void xv6_vfs_reset(void);

/**
 * @brief Open flag: read-only
 */
#define XV6_O_RDONLY 0x0000

/**
 * @brief Open flag: write-only
 */
#define XV6_O_WRONLY 0x0001

/**
 * @brief Open flag: read-write
 */
#define XV6_O_RDWR   0x0002

/**
 * @brief Mask for access mode bits
 */
#define XV6_O_ACCMODE 0x0003

/**
 * @brief Open flag: create if not exists
 */
#define XV6_O_CREAT  0x0200

/**
 * @brief Open flag: truncate to zero length
 */
#define XV6_O_TRUNC  0x0400

/**
 * @brief Open flag: append to end
 */
#define XV6_O_APPEND 0x0800

#endif
