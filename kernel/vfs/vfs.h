/**
 * @file vfs.h
 * @brief xv6 Virtual File System public API
 *
 * This is the main public API header for the xv6 VFS implementation.
 * Provides file operations, directory operations, and task management.
 */
#ifndef XV6_VFS_H
#define XV6_VFS_H

#include "core/types.h"

/**
 * @name Capacity Limits
 * @{
 */
#define XV6_TASK_CTX_CAP 64
#define XV6_FD_CAP 128
#define XV6_PTY_CAP 8
#define XV6_PIPE_CAP 64
/** @} */

/**
 * @name Open Flags
 * @{
 */
#define XV6_O_RDONLY 0x0000
#define XV6_O_WRONLY 0x0001
#define XV6_O_RDWR   0x0002
#define XV6_O_ACCMODE 0x0003
#define XV6_O_CREAT  0x0200
#define XV6_O_TRUNC  0x0400
#define XV6_O_APPEND 0x0800
/** @} */

/**
 * @brief File metadata structure
 *
 * Contains basic file metadata returned by stat/fstat.
 */
typedef struct {
  uint32 ino;     /**< Inode number */
  uint32 size;    /**< File size in bytes */
  uint16 type;    /**< File type (1=dir, 2=file, 3=device, 4=symlink) */
  uint16 nlink;   /**< Number of hard links */
  uint16 mode;    /**< Permission bits (low 12 bits) */
  uint16 uid;     /**< Owner id */
  uint16 gid;     /**< Group id */
} xv6_kstat_t;

/**
 * @brief Initialize the VFS layer
 * @return 0 on success, -1 on failure
 */
int xv6fs_ro_init(void);

/**
 * @brief Load a filesystem image from flash
 * @param image Pointer to filesystem image in flash
 * @param image_size Size of image in bytes
 * @return 0 on success, -1 on failure
 */
int xv6fs_ro_flash_image(const uint8 *image, uint32 image_size);

/**
 * @brief List files in root directory
 * @param index Entry index to retrieve
 * @param name_out Buffer for file name
 * @param name_out_len Size of name buffer
 * @param size_out Pointer for file size (optional)
 * @return 0 on success, -1 on error/EOF
 */
int xv6fs_ro_list(int index, char *name_out, int name_out_len, uint32 *size_out);

/**
 * @brief Read file into allocated memory
 * @param name File name in root
 * @param out_data Pointer to store allocated buffer
 * @param out_size Pointer to store file size
 * @return 0 on success, -1 on failure
 */
int xv6fs_ro_read_file_alloc(const char *name, void **out_data, uint32 *out_size);

/**
 * @brief Read file by path into allocated memory
 * @param path Full path to file
 * @param out_data Pointer to store allocated buffer
 * @param out_size Pointer to store file size
 * @return 0 on success, -1 on failure
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
 */
int xv6fs_list_path(const char *path, int index, char *name_out, int name_out_len, uint16 *type_out,
                    uint32 *size_out);

/**
 * @brief List directory contents by open descriptor
 * @param fd Directory file descriptor
 * @param index Entry index
 * @param name_out Buffer for entry name
 * @param name_out_len Buffer size
 * @param type_out Pointer for file type
 * @param size_out Pointer for file size
 * @return 0 on success, -1 on error/EOF
 */
int xv6fs_list_fd(int fd, int index, char *name_out, int name_out_len, uint16 *type_out, uint32 *size_out);

/**
 * @brief Write data to file (only for /dev/xxx special files)
 * @param path File path
 * @param data Data to write
 * @param size Size of data
 * @return Bytes written on success, -1 on failure
 */
int xv6fs_write_file_path(const char *path, const void *data, uint32 size);

/**
 * @brief Create directory
 * @param path Directory path to create
 * @return 0 on success, -1 on failure
 */
int xv6fs_mkdir_path(const char *path);

/**
 * @brief Remove file
 * @param path File path to remove
 * @return 0 on success, -1 on failure
 */
int xv6fs_unlink_path(const char *path);

/**
 * @brief Remove directory
 * @param path Directory path to remove
 * @return 0 on success, -1 on failure
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
 * @brief Create hard link
 * @param oldpath Existing path
 * @param newpath New link path
 * @return 0 on success, -1 on failure
 */
int xv6fs_link_path(const char *oldpath, const char *newpath);

/**
 * @brief Create symbolic link
 * @param target Link target as provided by caller
 * @param linkpath New symlink path
 * @return 0 on success, -1 on failure
 */
int xv6fs_symlink_path(const char *target, const char *linkpath);

/**
 * @brief Read symlink target
 * @param path Symlink path
 * @param buf Output buffer
 * @param bufsz Buffer size
 * @return Number of bytes copied on success, -1 on failure
 */
int xv6fs_readlink_path(const char *path, char *buf, uint32 bufsz);

/**
 * @brief Open file
 * @param path File path
 * @param flags Open flags (XV6_O_RDONLY, XV6_O_WRONLY, etc.)
 * @return File descriptor on success, -1 on failure
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
 * @brief Resize file referenced by descriptor
 * @param fd File descriptor
 * @param length New file length in bytes
 * @return 0 on success, -1 on failure
 */
int xv6_ftruncate(int fd, long long length);

/**
 * @brief Resize file referenced by path
 * @param path File path (final symlink followed)
 * @param length New file length in bytes
 * @return 0 on success, -1 on failure
 */
int xv6_truncate_path(const char *path, long long length);

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
 */
int xv6_ptsname(int master_fd, char *out_path, int out_len);

/**
 * @brief Create pipe
 * @param out_read_fd Pointer for read end fd
 * @param out_write_fd Pointer for write end fd
 * @return 0 on success, -1 on failure
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
 */
int xv6_chmod(const char *path, int mode);

/**
 * @brief Change owner/group of path
 * @param path File path
 * @param owner New owner or -1 to keep
 * @param group New group or -1 to keep
 * @param follow_final_nonzero Follow final symlink if non-zero
 * @return 0 on success, -1 on failure
 */
int xv6_chown_path(const char *path, int owner, int group, int follow_final_nonzero);

/**
 * @brief Change mode by file descriptor
 * @param fd File descriptor
 * @param mode New mode bits
 * @return 0 on success, -1 on failure
 */
int xv6_fchmod(int fd, int mode);

/**
 * @brief Change owner/group by file descriptor
 * @param fd File descriptor
 * @param owner New owner or -1 to keep
 * @param group New group or -1 to keep
 * @return 0 on success, -1 on failure
 */
int xv6_fchown(int fd, int owner, int group);

/**
 * @brief Get file status by path
 * @param path File path
 * @param st Buffer for metadata
 * @return 0 on success, -1 on failure
 */
int xv6_stat_path(const char *path, xv6_kstat_t *st);

/**
 * @brief Get file status by path without following final symlink
 * @param path File path
 * @param st Buffer for metadata
 * @return 0 on success, -1 on failure
 */
int xv6_lstat_path(const char *path, xv6_kstat_t *st);

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
 */
int xv6_last_errno(void);

/**
 * @brief Set stdio file descriptors
 * @param in_fd Stdin fd
 * @param out_fd Stdout fd
 * @param err_fd Stderr fd
 * @return 0 on success, -1 on failure
 */
int xv6_stdio_set_fds(int in_fd, int out_fd, int err_fd);

/**
 * @brief Reset stdio to defaults (0, 1, 2)
 */
void xv6_stdio_reset_fds(void);

/**
 * @brief Get current stdio mapping for this task
 * @param out_in_fd Optional stdin fd output
 * @param out_out_fd Optional stdout fd output
 * @param out_err_fd Optional stderr fd output
 * @param out_active Optional stdio-active flag output (1 if custom mapping is active)
 *
 * Returns task-local stdio mapping. If no task context exists, defaults to
 * 0/1/2 with inactive mapping.
 */
void xv6_stdio_get_fds(int *out_in_fd, int *out_out_fd, int *out_err_fd, int *out_active);

/**
 * @brief Check if default stdout is active
 * @return 1 if default stdout, 0 otherwise
 */
int xv6_stdio_is_default_out(void);

/**
 * @brief Cleanup task context
 */
void xv6_task_ctx_cleanup(void);

/**
 * @brief Cleanup context for specific task
 * @param task_handle FreeRTOS task handle
 */
void xv6_task_ctx_cleanup_for_handle(void *task_handle);

/**
 * @brief Reset entire VFS state
 */
void xv6_vfs_reset(void);

#endif
