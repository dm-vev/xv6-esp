/**
 * @file hostabi_dirent.h
 * @brief POSIX directory entry API for xv6 host environment
 *
 * This header provides directory streaming operations for reading directory
 * contents in a POSIX-compatible way. Supports path-based and fd-based iteration.
 */
#ifndef XV6_HOSTABI_DIRENT_H
#define XV6_HOSTABI_DIRENT_H

#include <dirent.h>

/**
 * @brief Open a directory for reading
 * @param path Directory path to open
 * @return DIR pointer on success, NULL on failure
 *
 * Opens a directory and returns a directory stream handle. Uses xv6fs_list_path
 * internally for iteration, so the returned DIR* is path-based.
 * @post On failure, errno is set appropriately
 */
DIR *hostabi_opendir(const char *path);

/**
 * @brief Read next directory entry
 * @param dirp Directory stream
 * @return Pointer to dirent on success, NULL on EOF or error
 *
 * Reads the next directory entry from the directory stream. Each call
 * advances the internal position counter.
 * @pre dirp != NULL
 * @post On error, errno may be set
 */
struct dirent *hostabi_readdir(DIR *dirp);

/**
 * @brief Close a directory stream
 * @param dirp Directory stream to close
 * @return 0 on success, -1 on failure
 *
 * Closes the directory stream and frees associated resources.
 */
int hostabi_closedir(DIR *dirp);

/**
 * @brief Rewind directory stream to beginning
 * @param dirp Directory stream
 *
 * Resets the directory stream to the beginning, so the next call
 * to readdir() returns the first entry.
 */
void hostabi_rewinddir(DIR *dirp);

/**
 * @brief Get file descriptor for directory stream
 * @param dirp Directory stream
 * @return File descriptor on success, -1 on failure
 *
 * Returns the underlying file descriptor associated with the
 * directory stream. May fail if the directory was opened in a
 * mode that doesn't use file descriptors.
 */
int hostabi_dirfd(DIR *dirp);

/**
 * @brief Open directory from file descriptor
 * @param fd File descriptor
 * @return DIR pointer on success, NULL on failure
 *
 * Opens a directory from an existing file descriptor. On success,
 * the returned DIR stream owns the descriptor and closedir() closes it.
 */
DIR *hostabi_fdopendir(int fd);

#endif
