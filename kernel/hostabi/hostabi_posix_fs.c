/**
 * @file hostabi_posix_fs.c
 * @brief Implementation of POSIX file system operations for xv6 host environment
 *
 * This file implements the POSIX file system API that bridges between the
 * host environment and xv6's virtual file system. It handles:
 * - Flag translation from POSIX to xv6 conventions
 * - Path translation for guest memory pointers
 * - Error mapping from xv6 errno to POSIX errno
 * - File descriptor operations (open, read, write, close, dup, etc.)
 */
#include "hostabi/hostabi_posix_fs.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include "loader/elf_loader.h"
#include "hostabi/hostabi_posix_io.h"
#include "vfs/xv6fs_ro.h"

#define XV6_KSTAT_T_DIR 1
#define XV6_KSTAT_T_DEVICE 3

/**
 * @brief Map xv6 errno to POSIX errno with fallback
 * @param fallback Errno value to use if xv6 errno is invalid
 * @return -1 (always returns -1 to indicate error)
 *
 * Retrieves the last xv6 errno and maps it to a POSIX-compatible value.
 * If xv6 didn't set an errno, uses the provided fallback.
 */
static int set_errno_from_xv6_or(int fallback)
{
  int err = xv6_last_errno();
  if(err <= 0)
    err = fallback;
  errno = err;
  return -1;
}

/**
 * @brief Translate POSIX open flags to xv6 flags
 * @param flags POSIX open flags
 * @return xv6 open flags
 *
 * Converts POSIX O_RDONLY/O_WRONLY/O_RDWR to xv6 equivalents and maps
 * O_CREAT, O_TRUNC, O_APPEND to their xv6 counterparts.
 */
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

/**
 * @brief Convert xv6 file type to POSIX mode
 * @param type xv6 stat type value
 * @return POSIX mode with file type
 *
 * Maps xv6 stat types to POSIX file types:
 * - XV6_KSTAT_T_DIR -> S_IFDIR
 * - XV6_KSTAT_T_DEVICE -> S_IFCHR
 * - otherwise -> S_IFREG
 */
static mode_t mode_from_xv6_type(uint16 type)
{
  if(type == XV6_KSTAT_T_DIR)
    return (mode_t)(S_IFDIR | 0777);
  if(type == XV6_KSTAT_T_DEVICE)
    return (mode_t)(S_IFCHR | 0666);
  return (mode_t)(S_IFREG | 0666);
}

/**
 * @brief Fill POSIX stat structure from xv6 kstat
 * @param kst xv6 kstat structure
 * @param st POSIX stat structure to fill
 * @return 0 on success, -1 on failure
 *
 * Copies relevant fields from xv6's kstat to POSIX stat, converting
 * types appropriately. Sets st_nlink to 1 if zero in xv6 (for files
 * without explicit link count).
 */
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

/**
 * @brief Map file descriptor between host and xv6 layers
 * @param fd File descriptor
 * @return Mapped descriptor
 *
 * Currently a pass-through function that allows future mapping
 * between host and xv6 descriptor spaces if needed.
 */
int hostabi_posix_fs_map_fd(int fd)
{
  return fd;
}

/**
 * @brief Open a file with specified mode
 * @param path File path
 * @param flags Open flags
 * @param mode File mode (unused in implementation)
 * @return File descriptor on success, -1 on failure
 *
 * Translates path from guest memory, maps flags to xv6 format,
 * calls xv6_open, and registers the fd with the I/O subsystem.
 */
int hostabi_posix_fs_open_mode(const char *path, int flags, mode_t mode)
{
  int fd;
  (void)mode;

  /* Translate path from guest memory address space */
  path = (const char *)elf_loader_translate_ptr(path);
  if(path == 0){
    errno = EINVAL;
    return -1;
  }

  fd = xv6_open(path, map_open_flags(flags));
  if(fd < 0)
    return set_errno_from_xv6_or(ENOENT);

  /* Register fd metadata for TTY detection, etc. */
  hostabi_posix_io_on_open(fd, flags, path);
  return fd;
}

/**
 * @brief Create a new file
 * @param path File path
 * @param mode File permissions
 * @return File descriptor on success, -1 on failure
 *
 * Convenience wrapper that calls open with O_CREAT|O_TRUNC|O_WRONLY.
 */
int hostabi_posix_fs_creat(const char *path, mode_t mode)
{
  return hostabi_posix_fs_open_mode(path, O_CREAT | O_TRUNC | O_WRONLY, mode);
}

/**
 * @brief Read from a file descriptor
 * @param fd File descriptor
 * @param buf Buffer to read into
 * @param size Number of bytes to read
 * @return Bytes read on success, 0 on EOF, -1 on failure
 *
 * Maps fd, validates buffer pointer from guest memory, and calls
 * xv6_read. Returns 0 for zero-size reads without calling xv6.
 */
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

/**
 * @brief Write to a file descriptor
 * @param fd File descriptor
 * @param buf Buffer containing data
 * @param size Number of bytes to write
 * @return Bytes written on success, -1 on failure
 *
 * Maps fd, validates buffer pointer from guest memory, and calls
 * xv6_write. Returns 0 for zero-size writes without calling xv6.
 */
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

/**
 * @brief Close a file descriptor
 * @param fd File descriptor to close
 * @return 0 on success, -1 on failure
 *
 * Calls xv6_close and notifies I/O subsystem to clean up metadata.
 */
int hostabi_posix_fs_close(int fd)
{
  fd = hostabi_posix_fs_map_fd(fd);
  if(xv6_close(fd) != 0)
    return set_errno_from_xv6_or(EBADF);

  hostabi_posix_io_on_close(fd, 0);
  return 0;
}

/**
 * @brief Duplicate a file descriptor
 * @param fd File descriptor to duplicate
 * @return New file descriptor on success, -1 on failure
 *
 * Creates a new fd pointing to the same file description.
 */
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

/**
 * @brief Duplicate to specific file descriptor
 * @param oldfd Original fd
 * @param newfd Target fd
 * @return newfd on success, -1 on failure
 *
 * Complex implementation that handles:
 * - Same fd values (return immediately)
 * - Closing target if already open
 * - Preserving errno on failure
 * - Rolling back state on failure
 */
int hostabi_posix_fs_dup2(int oldfd, int newfd)
{
  int dups[XV6_FD_CAP];
  int ndups = 0;
  int restore_dups[XV6_FD_CAP];
  int nrestore_dups = 0;
  int rc;
  int i;
  xv6_kstat_t st;
  xv6_kstat_t newst;
  int had_newfd = 0;
  int backup_fd = -1;
  int restore_ok = 0;

  oldfd = hostabi_posix_fs_map_fd(oldfd);
  newfd = hostabi_posix_fs_map_fd(newfd);

  if(newfd < 0){
    errno = EBADF;
    return -1;
  }
  if(xv6_fstat(oldfd, &st) != 0)
    return set_errno_from_xv6_or(EBADF);

  /* If same fd, just notify and return */
  if(oldfd == newfd){
    hostabi_posix_io_on_dup(oldfd, newfd);
    return newfd;
  }

  /* Backup target if it's open */
  if(xv6_fstat(newfd, &newst) == 0){
    had_newfd = 1;
    backup_fd = xv6_dup(newfd);
    if(backup_fd < 0)
      return set_errno_from_xv6_or(EMFILE);
    hostabi_posix_io_on_dup(newfd, backup_fd);
    (void)xv6_close(newfd);
    hostabi_posix_io_on_close(newfd, 0);
  }

  /* Loop until we get the exact fd we want */
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

  /* Clean up intermediate fds */
  for(i = 0; i < ndups; i++){
    (void)xv6_close(dups[i]);
    hostabi_posix_io_on_close(dups[i], 0);
  }
  if(backup_fd >= 0){
    (void)xv6_close(backup_fd);
    hostabi_posix_io_on_close(backup_fd, 0);
  }
  return rc;

fail:
  /* Clean up intermediate fds from failed attempt */
  for(i = 0; i < ndups; i++){
    (void)xv6_close(dups[i]);
    hostabi_posix_io_on_close(dups[i], 0);
  }

  /* Try to restore original state if we backed up newfd */
  if(had_newfd && backup_fd >= 0){
    while(1){
      rc = xv6_dup(backup_fd);
      if(rc < 0)
        break;
      hostabi_posix_io_on_dup(backup_fd, rc);
      if(rc == newfd){
        restore_ok = 1;
        break;
      }
      if(nrestore_dups >= (int)(sizeof(restore_dups) / sizeof(restore_dups[0]))){
        errno = EMFILE;
        break;
      }
      restore_dups[nrestore_dups++] = rc;
    }
    for(i = 0; i < nrestore_dups; i++){
      (void)xv6_close(restore_dups[i]);
      hostabi_posix_io_on_close(restore_dups[i], 0);
    }
    (void)xv6_close(backup_fd);
    hostabi_posix_io_on_close(backup_fd, 0);
    if(!restore_ok && errno == 0)
      errno = EMFILE;
  }

  if(errno == 0)
    (void)set_errno_from_xv6_or(EBADF);
  return -1;
}

/**
 * @brief Create a pipe
 * @param pipefd Array[2] for read/write fds
 * @return 0 on success, -1 on failure
 *
 * Creates a unidirectional pipe via xv6_pipe, stores the two fds,
 * and registers both with the I/O subsystem.
 */
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

/**
 * @brief Change file offset
 * @param fd File descriptor
 * @param offset Offset
 * @param whence SEEK_SET, SEEK_CUR, or SEEK_END
 * @return New offset on success, -1 on failure
 *
 * Repositions the file offset using xv6_lseek.
 */
off_t hostabi_posix_fs_lseek(int fd, off_t offset, int whence)
{
  int rc;

  fd = hostabi_posix_fs_map_fd(fd);
  rc = xv6_lseek(fd, (int)offset, whence);
  if(rc < 0)
    return (off_t)set_errno_from_xv6_or(EINVAL);

  return (off_t)rc;
}

/**
 * @brief Get file status by fd
 * @param fd File descriptor
 * @param st Buffer for status
 * @return 0 on success, -1 on failure
 *
 * Gets file metadata via xv6_fstat and fills POSIX stat structure.
 */
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

/**
 * @brief Get file status by path
 * @param path File path
 * @param st Buffer for status
 * @return 0 on success, -1 on failure
 *
 * Gets file metadata via xv6_stat_path and fills POSIX stat structure.
 */
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

/**
 * @brief Get file status (no symlink follow)
 * @see hostabi_posix_fs_stat
 */
int hostabi_posix_fs_lstat(const char *path, struct stat *st)
{
  return hostabi_posix_fs_stat(path, st);
}

/**
 * @brief Read symbolic link
 * @param path Link path
 * @param buf Buffer for target
 * @param bufsz Buffer size
 * @return -1 (not implemented)
 *
 * Not implemented - returns ENOSYS.
 */
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
