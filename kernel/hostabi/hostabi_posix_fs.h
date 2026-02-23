/**
 * @file hostabi_posix_fs.h
 * @brief POSIX file system API for xv6 host environment
 *
 * This header provides POSIX-compliant file system operations that bridge
 * between the host environment and the xv6 virtual file system. It handles
 * flag translation, path translation, and errno mapping between layers.
 */
#ifndef XV6_HOSTABI_POSIX_FS_H
#define XV6_HOSTABI_POSIX_FS_H

#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

/**
 * @brief Map a file descriptor between host and xv6 layers
 * @param fd File descriptor from host layer
 * @return Mapped file descriptor for xv6 layer
 *
 * Provides a hook for descriptor value mapping between newlib and xv6
 * layers. Currently returns the descriptor unchanged.
 */
int hostabi_posix_fs_map_fd(int fd);

/**
 * @brief Open a file with specified mode
 * @param path File path (translated via elf_loader_translate_ptr)
 * @param flags POSIX open flags
 * @param mode File mode for creation
 * @return File descriptor on success, -1 on failure
 *
 * Opens a file in the xv6 file system with flag translation from POSIX
 * to xv6 flags. Translates paths and propagates errors via errno.
 * @pre path != NULL
 * @post On failure, errno is set appropriately
 */
int hostabi_posix_fs_open_mode(const char *path, int flags, mode_t mode);

/**
 * @brief Create a new file
 * @param path File path to create
 * @param mode File permissions
 * @return File descriptor on success, -1 on failure
 *
 * Equivalent to open(path, O_CREAT | O_TRUNC | O_WRONLY, mode).
 */
int hostabi_posix_fs_creat(const char *path, mode_t mode);

/**
 * @brief Read from a file descriptor
 * @param fd File descriptor
 * @param buf Buffer to read into (translated via elf_loader_translate_ptr)
 * @param size Number of bytes to read
 * @return Bytes read on success, 0 on EOF, -1 on failure
 *
 * Reads data from an xv6 file descriptor into the provided buffer.
 * Handles pointer translation for guest memory addresses.
 */
int hostabi_posix_fs_read(int fd, void *buf, size_t size);

/**
 * @brief Write to a file descriptor
 * @param fd File descriptor
 * @param buf Buffer containing data to write (translated via elf_loader_translate_ptr)
 * @param size Number of bytes to write
 * @return Bytes written on success, -1 on failure
 *
 * Writes data to an xv6 file descriptor from the provided buffer.
 * Handles pointer translation for guest memory addresses.
 */
int hostabi_posix_fs_write(int fd, const void *buf, size_t size);

/**
 * @brief Close a file descriptor
 * @param fd File descriptor to close
 * @return 0 on success, -1 on failure
 *
 * Closes an xv6 file descriptor and releases associated resources.
 */
int hostabi_posix_fs_close(int fd);

/**
 * @brief Duplicate a file descriptor
 * @param fd File descriptor to duplicate
 * @return New file descriptor on success, -1 on failure
 *
 * Creates a new file descriptor pointing to the same file description.
 */
int hostabi_posix_fs_dup(int fd);

/**
 * @brief Duplicate to a specific file descriptor
 * @param oldfd Original file descriptor
 * @param newfd Target file descriptor
 * @return newfd on success, -1 on failure
 *
 * Ensures newfd refers to the same file as oldfd, closing newfd first
 * if it was already open. Implements complex dup2 semantics with
 * proper error handling and state preservation.
 */
int hostabi_posix_fs_dup2(int oldfd, int newfd);

/**
 * @brief Create a pipe
 * @param pipefd Array[2] to store read/write file descriptors
 * @return 0 on success, -1 on failure
 *
 * Creates a unidirectional pipe and stores read end in pipefd[0]
 * and write end in pipefd[1].
 */
int hostabi_posix_fs_pipe(int pipefd[2]);

/**
 * @brief Change file offset
 * @param fd File descriptor
 * @param offset Offset relative to whence
 * @param whence SEEK_SET, SEEK_CUR, or SEEK_END
 * @return New offset on success, -1 on failure
 *
 * Repositions the file offset for the given file descriptor.
 */
off_t hostabi_posix_fs_lseek(int fd, off_t offset, int whence);

/**
 * @brief Get file status by descriptor
 * @param fd File descriptor
 * @param st Buffer to store file status
 * @return 0 on success, -1 on failure
 *
 * Retrieves metadata about the file associated with fd.
 */
int hostabi_posix_fs_fstat(int fd, struct stat *st);

/**
 * @brief Get file status by path
 * @param path File path
 * @param st Buffer to store file status
 * @return 0 on success, -1 on failure
 *
 * Retrieves metadata about the file at the given path.
 */
int hostabi_posix_fs_stat(const char *path, struct stat *st);

/**
 * @brief Get file status by path (no symlink follow)
 * @param path File path
 * @param st Buffer to store file status
 * @return 0 on success, -1 on failure
 *
 * Like stat() but does not follow symbolic links.
 */
int hostabi_posix_fs_lstat(const char *path, struct stat *st);

/**
 * @brief Read value of a symbolic link
 * @param path Symbolic link path
 * @param buf Buffer for link target
 * @param bufsz Size of buffer
 * @return Bytes written on success, -1 on failure
 */
int hostabi_posix_fs_readlink(const char *path, char *buf, size_t bufsz);

#endif
