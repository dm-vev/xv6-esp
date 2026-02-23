/**
 * @file hostabi_pty.h
 * @brief POSIX pseudo-terminal (PTY) API for xv6 host environment
 *
 * This header provides pseudo-terminal functionality, allowing creation
 * of terminal pairs for implementing features like shells and remote access.
 */
#ifndef XV6_HOSTABI_PTY_H
#define XV6_HOSTABI_PTY_H

#include <stddef.h>

/**
 * @brief Open a pseudo-terminal master device
 * @param flags Open flags (only O_RDWR and O_NOCTTY, O_CLOEXEC are allowed)
 * @return File descriptor for PTY master on success, -1 on failure
 *
 * Opens /dev/ptmx and creates a pseudo-terminal pair. Returns the
 * master side file descriptor.
 * @pre flags must include O_RDWR
 * @pre flags must not include any flags besides O_NOCTTY and O_CLOEXEC
 * @post On failure, errno is set appropriately
 */
int hostabi_posix_openpt(int flags);

/**
 * @brief Grant access to a pseudo-terminal slave
 * @param fd File descriptor for PTY master
 * @return 0 on success, -1 on failure
 *
 * Sets the ownership and permissions of the PTY slave device.
 * In xv6 environment, validates that fd is a valid PTY.
 */
int hostabi_grantpt(int fd);

/**
 * @brief Unlock a pseudo-terminal slave
 * @param fd File descriptor for PTY master
 * @return 0 on success, -1 on failure
 *
 * Unlocks the PTY slave device, allowing it to be opened.
 */
int hostabi_unlockpt(int fd);

/**
 * @brief Get PTY slave name (thread-unsafe)
 * @param fd File descriptor for PTY master
 * @return Pointer to static buffer with slave name, or NULL on failure
 *
 * Returns the path to the PTY slave device (e.g., /dev/pts/0).
 * WARNING: Not thread-safe - uses a static buffer.
 * @see hostabi_ptsname_r for thread-safe version
 */
char *hostabi_ptsname(int fd);

/**
 * @brief Get PTY slave name (thread-safe)
 * @param fd File descriptor for PTY master
 * @param buf Buffer to store slave name
 * @param buflen Size of buffer
 * @return 0 on success, error code on failure
 *
 * Thread-safe version of ptsname that stores the result in the
 * provided buffer.
 * @pre buf != NULL && buflen > 0
 * @return EINVAL if fd is invalid, ERANGE if buffer too small
 */
int hostabi_ptsname_r(int fd, char *buf, size_t buflen);

#endif
