#include "hostabi_pty.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include "elf_loader.h"
#include "hostabi_posix_fs.h"
#include "param.h"
#include "xv6fs_ro.h"

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

int hostabi_ptsname_r(int fd, char *buf, size_t buflen)
{
  char tmp[MAXPATH];
  size_t n;

  buf = (char *)elf_loader_translate_ptr(buf);
  if(buf == 0 || buflen == 0){
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
  if(n > buflen){
    errno = ERANGE;
    return ERANGE;
  }
  memcpy(buf, tmp, n);
  return 0;
}

char *hostabi_ptsname(int fd)
{
  static char buf[MAXPATH];
  if(hostabi_ptsname_r(fd, buf, sizeof(buf)) != 0)
    return 0;
  return buf;
}
