/**
 * @file hostabi_pty.c
 * @brief Implementation of POSIX pseudo-terminal operations
 *
 * This file implements pseudo-terminal (PTY) functions that allow creation
 * of terminal pairs for inter-process communication. PTYs consist of a
 * master device (/dev/ptmx) and a slave device (e.g., /dev/pts/0).
 */
#include "hostabi/hostabi_pty.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include "loader/elf_loader.h"
#include "hostabi/hostabi_posix_fs.h"
#include "core/param.h"
#include "vfs/xv6fs_ro.h"

/**
 * @brief Open a pseudo-terminal master device
 * @param flags Open flags (must include O_RDWR)
 * @return PTY master fd on success, -1 on failure
 *
 * Validates flags to ensure O_RDWR (required) and rejects unsupported flags,
 * then opens /dev/ptmx to create a PTY pair.
 */
int hostabi_posix_openpt(int flags)
{
  int accmode = (flags & O_ACCMODE);
  int allowed = (O_RDWR
#ifdef O_NOCTTY
                 | O_NOCTTY
#endif
#ifdef O_CLOEXEC
                 | O_CLOEXEC
#endif
  );
  if(accmode != O_RDWR){
    errno = EINVAL;
    return -1;
  }
  if((flags & ~allowed) != 0){
    errno = EINVAL;
    return -1;
  }

  int fd = hostabi_posix_fs_open_mode("/dev/ptmx", flags, 0);
  if(fd < 0)
    return -1;
  return fd;
}

/**
 * @brief Grant access to PTY slave
 * @param fd PTY master fd
 * @return 0 on success, -1 on failure
 *
 * Validates the fd and retrieves the slave name via xv6_ptsname.
 * In this implementation, just validates the fd is a valid PTY.
 */
int hostabi_grantpt(int fd)
{
  char pbuf[MAXPATH];
  if(xv6_ptsname(fd, pbuf, sizeof(pbuf)) != 0){
    errno = xv6_last_errno();
    if(errno <= 0)
      errno = EBADF;
    return -1;
  }
  return 0;
}

/**
 * @brief Unlock PTY slave
 * @param fd PTY master fd
 * @return 0 on success, -1 on failure
 *
 * Unlocks the PTY slave device, allowing it to be opened by other processes.
 * Validates fd by checking if ptsname succeeds.
 */
int hostabi_unlockpt(int fd)
{
  char pbuf[MAXPATH];
  if(xv6_ptsname(fd, pbuf, sizeof(pbuf)) != 0){
    errno = xv6_last_errno();
    if(errno <= 0)
      errno = EBADF;
    return -1;
  }
  return 0;
}

/**
 * @brief Get PTY slave name (thread-safe)
 * @param fd PTY master fd
 * @param buf Buffer for slave name
 * @param bufsz Buffer size
 * @return 0 on success, error code on failure
 *
 * Retrieves the path to the PTY slave device. Validates buffer pointer,
 * calls xv6_ptsname, and checks that the name fits in the provided buffer.
 */
int hostabi_ptsname_r(int fd, char *buf, size_t bufsz)
{
  char tmp[MAXPATH];
  size_t n;

  buf = (char *)elf_loader_translate_ptr(buf);
  if(buf == 0 || bufsz == 0){
    errno = EINVAL;
    return EINVAL;
  }
  if(xv6_ptsname(fd, tmp, sizeof(tmp)) != 0){
    errno = xv6_last_errno();
    if(errno <= 0)
      errno = EINVAL;
    return errno;
  }
  n = strlen(tmp) + 1u;
  if(n > bufsz){
    errno = ERANGE;
    return ERANGE;
  }
  memcpy(buf, tmp, n);
  return 0;
}

/**
 * @brief Get PTY slave name (thread-unsafe)
 * @param fd PTY master fd
 * @return Pointer to static buffer, or NULL on failure
 *
 * Thread-unsafe wrapper around ptsname_r that uses a static buffer.
 * Not safe for multi-threaded use - use ptsname_r instead.
 */
char *hostabi_ptsname(int fd)
{
  static char buf[MAXPATH];
  if(hostabi_ptsname_r(fd, buf, sizeof(buf)) != 0)
    return 0;
  return buf;
}
