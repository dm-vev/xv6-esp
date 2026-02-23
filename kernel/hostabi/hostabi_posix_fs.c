#include "hostabi/hostabi_posix_fs.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include "loader/elf_loader.h"
#include "hostabi/hostabi_posix_io.h"
#include "vfs/xv6fs_ro.h"

#define XV6_KSTAT_T_DIR 1
#define XV6_KSTAT_T_DEVICE 3

static int set_errno_from_xv6_or(int fallback)
{
  int err = xv6_last_errno();
  if(err <= 0)
    err = fallback;
  errno = err;
  return -1;
}

static int map_open_flags(int flags)
{
  int xv6_flags = 0;
  switch(flags & O_ACCMODE){
  case O_WRONLY:
    xv6_flags |= XV6_O_WRONLY;
    break;
  case O_RDWR:
    xv6_flags |= XV6_O_RDWR;
    break;
  default:
    xv6_flags |= XV6_O_RDONLY;
    break;
  }
  if(flags & O_CREAT)
    xv6_flags |= XV6_O_CREAT;
  if(flags & O_TRUNC)
    xv6_flags |= XV6_O_TRUNC;
  if(flags & O_APPEND)
    xv6_flags |= XV6_O_APPEND;
  return xv6_flags;
}

static mode_t mode_from_xv6_type(uint16 type)
{
  if(type == XV6_KSTAT_T_DIR)
    return (mode_t)(S_IFDIR | 0777);
  if(type == XV6_KSTAT_T_DEVICE)
    return (mode_t)(S_IFCHR | 0666);
  return (mode_t)(S_IFREG | 0666);
}

static int fill_host_stat(const xv6_kstat_t *kst, struct stat *st)
{
  if(kst == 0 || st == 0)
    return -1;
  memset(st, 0, sizeof(*st));
  st->st_ino = (ino_t)kst->ino;
  st->st_nlink = (nlink_t)(kst->nlink ? kst->nlink : 1);
  st->st_mode = mode_from_xv6_type(kst->type);
  st->st_size = (off_t)kst->size;
  return 0;
}

int hostabi_posix_fs_map_fd(int fd)
{
  /* Keep descriptor values untouched between newlib and xv6 layers. */
  return fd;
}

int hostabi_posix_fs_open_mode(const char *path, int flags, mode_t mode)
{
  int fd;
  (void)mode;

  path = (const char *)elf_loader_translate_ptr(path);
  if(path == 0){
    errno = EINVAL;
    return -1;
  }

  fd = xv6_open(path, map_open_flags(flags));
  if(fd < 0)
    return set_errno_from_xv6_or(ENOENT);

  hostabi_posix_io_on_open(fd, flags, path);
  return fd;
}

int hostabi_posix_fs_creat(const char *path, mode_t mode)
{
  return hostabi_posix_fs_open_mode(path, O_CREAT | O_TRUNC | O_WRONLY, mode);
}

int hostabi_posix_fs_read(int fd, void *buf, size_t size)
{
  int rc;

  fd = hostabi_posix_fs_map_fd(fd);
  if(size == 0)
    return 0;
  buf = (void *)elf_loader_translate_ptr(buf);
  if(buf == 0){
    errno = EINVAL;
    return -1;
  }

  rc = xv6_read(fd, buf, (uint32)size);
  if(rc < 0)
    return set_errno_from_xv6_or(EIO);
  return rc;
}

int hostabi_posix_fs_write(int fd, const void *buf, size_t size)
{
  int rc;

  fd = hostabi_posix_fs_map_fd(fd);
  if(size == 0)
    return 0;
  buf = elf_loader_translate_ptr(buf);
  if(buf == 0){
    errno = EINVAL;
    return -1;
  }

  rc = xv6_write(fd, buf, (uint32)size);
  if(rc < 0)
    return set_errno_from_xv6_or(EIO);
  return rc;
}

int hostabi_posix_fs_close(int fd)
{
  fd = hostabi_posix_fs_map_fd(fd);
  if(xv6_close(fd) != 0)
    return set_errno_from_xv6_or(EBADF);

  hostabi_posix_io_on_close(fd, 0);
  return 0;
}

int hostabi_posix_fs_dup(int fd)
{
  int rc;

  fd = hostabi_posix_fs_map_fd(fd);
  rc = xv6_dup(fd);
  if(rc < 0)
    return set_errno_from_xv6_or(EBADF);

  hostabi_posix_io_on_dup(fd, rc);
  return rc;
}

int hostabi_posix_fs_dup2(int oldfd, int newfd)
{
  int dups[XV6_FD_CAP];
  int ndups = 0;
  int rc;
  int i;
  xv6_kstat_t st;

  oldfd = hostabi_posix_fs_map_fd(oldfd);
  newfd = hostabi_posix_fs_map_fd(newfd);

  if(newfd < 0){
    errno = EBADF;
    return -1;
  }
  if(xv6_fstat(oldfd, &st) != 0)
    return set_errno_from_xv6_or(EBADF);

  if(oldfd == newfd){
    hostabi_posix_io_on_dup(oldfd, newfd);
    return newfd;
  }

  (void)hostabi_posix_fs_close(newfd);

  errno = 0;
  while(1){
    rc = xv6_dup(oldfd);
    if(rc < 0)
      goto fail;
    hostabi_posix_io_on_dup(oldfd, rc);
    if(rc == newfd)
      break;
    if(ndups >= (int)(sizeof(dups) / sizeof(dups[0]))){
      errno = EMFILE;
      goto fail;
    }
    dups[ndups++] = rc;
  }

  for(i = 0; i < ndups; i++){
    (void)xv6_close(dups[i]);
    hostabi_posix_io_on_close(dups[i], 0);
  }
  return rc;

fail:
  for(i = 0; i < ndups; i++){
    (void)xv6_close(dups[i]);
    hostabi_posix_io_on_close(dups[i], 0);
  }
  if(errno == 0)
    (void)set_errno_from_xv6_or(EBADF);
  return -1;
}

int hostabi_posix_fs_pipe(int pipefd[2])
{
  int *fds;
  int rfd;
  int wfd;

  fds = (int *)elf_loader_translate_ptr(pipefd);
  if(fds == 0)
    fds = pipefd;
  if(fds == 0){
    errno = EINVAL;
    return -1;
  }

  if(xv6_pipe(&rfd, &wfd) != 0)
    return set_errno_from_xv6_or(EMFILE);

  fds[0] = rfd;
  fds[1] = wfd;
  hostabi_posix_io_on_open(rfd, O_RDONLY, "/dev/pipe");
  hostabi_posix_io_on_open(wfd, O_WRONLY, "/dev/pipe");
  return 0;
}

off_t hostabi_posix_fs_lseek(int fd, off_t offset, int whence)
{
  int rc;

  fd = hostabi_posix_fs_map_fd(fd);
  rc = xv6_lseek(fd, (int)offset, whence);
  if(rc < 0)
    return (off_t)set_errno_from_xv6_or(EINVAL);

  return (off_t)rc;
}

int hostabi_posix_fs_fstat(int fd, struct stat *st)
{
  xv6_kstat_t kst;

  fd = hostabi_posix_fs_map_fd(fd);
  st = (struct stat *)elf_loader_translate_ptr(st);
  if(st == 0){
    errno = EINVAL;
    return -1;
  }

  if(xv6_fstat(fd, &kst) != 0)
    return set_errno_from_xv6_or(EBADF);
  if(fill_host_stat(&kst, st) != 0){
    errno = EIO;
    return -1;
  }
  return 0;
}

int hostabi_posix_fs_stat(const char *path, struct stat *st)
{
  xv6_kstat_t kst;

  path = (const char *)elf_loader_translate_ptr(path);
  st = (struct stat *)elf_loader_translate_ptr(st);
  if(path == 0 || st == 0){
    errno = EINVAL;
    return -1;
  }

  if(xv6_stat_path(path, &kst) != 0)
    return set_errno_from_xv6_or(ENOENT);
  if(fill_host_stat(&kst, st) != 0){
    errno = EIO;
    return -1;
  }
  return 0;
}

int hostabi_posix_fs_lstat(const char *path, struct stat *st)
{
  return hostabi_posix_fs_stat(path, st);
}

int hostabi_posix_fs_readlink(const char *path, char *buf, size_t bufsz)
{
  path = (const char *)elf_loader_translate_ptr(path);
  (void)buf;
  (void)bufsz;
  if(path == 0){
    errno = EINVAL;
    return -1;
  }
  errno = ENOSYS;
  return -1;
}
