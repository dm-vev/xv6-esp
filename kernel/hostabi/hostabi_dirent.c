/**
 * @file hostabi_dirent.c
 * @brief Implementation of POSIX directory entry operations
 *
 * This file implements directory streaming operations for reading directory
 * contents. Uses xv6's path-based iteration (xv6fs_list_path) internally,
 * making it robust even when the underlying filesystem doesn't support
 * directory file descriptors.
 */
#include "hostabi/hostabi_dirent.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "loader/elf_loader.h"
#include "hostabi/hostabi_posix_fs.h"
#include "core/param.h"
#include "vfs/vfs.h"

#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#endif
#ifndef DT_DIR
#define DT_DIR 4
#endif
#ifndef DT_CHR
#define DT_CHR 2
#endif
#ifndef DT_REG
#define DT_REG 8
#endif
#ifndef DT_LNK
#define DT_LNK 10
#endif

#define XV6_KSTAT_T_DIR 1
#define XV6_KSTAT_T_FILE 2
#define XV6_KSTAT_T_DEVICE 3
#define XV6_KSTAT_T_SYMLINK 4
#define HOSTABI_DIR_MAGIC 0x48445231u

/**
 * @brief Internal directory handle structure
 *
 * Stores state for iterating through a directory, including:
 * - Magic number for validation
 * - File descriptor (may be -1 if not used)
 * - Current index in directory
 * - Original path for iteration
 * - Current dirent buffer
 */
typedef struct {
  uint32 magic;        /**< Validation magic (HOSTABI_DIR_MAGIC) */
  int fd;             /**< Directory fd (may be -1) */
  int index;          /**< Current entry index */
  char path[MAXPATH]; /**< Original directory path */
  struct dirent ent;  /**< Current directory entry */
} hostabi_dir_t;

/**
 * @brief Safe string copy with bounds checking
 * @param dst Destination buffer
 * @param dst_len Size of destination
 * @param src Source string (can be NULL)
 */
static void copy_cstr(char *dst, int dst_len, const char *src)
{
  if(dst == 0 || dst_len <= 0)
    return;
  if(src == 0)
    src = "";
  strncpy(dst, src, (size_t)dst_len - 1u);
  dst[dst_len - 1] = 0;
}

/**
 * @brief Convert xv6 stat type to dirent type
 * @param type xv6 stat type
 * @return dirent d_type value
 *
 * Maps:
 * - XV6_KSTAT_T_DIR -> DT_DIR
 * - XV6_KSTAT_T_FILE -> DT_REG
 * - XV6_KSTAT_T_DEVICE -> DT_CHR
 * - otherwise -> DT_UNKNOWN
 */
static int dirent_type_from_xv6(uint16 type)
{
  if(type == XV6_KSTAT_T_DIR)
    return DT_DIR;
  if(type == XV6_KSTAT_T_FILE)
    return DT_REG;
  if(type == XV6_KSTAT_T_DEVICE)
    return DT_CHR;
  if(type == XV6_KSTAT_T_SYMLINK)
    return DT_LNK;
  return DT_UNKNOWN;
}

/**
 * @brief Open a directory
 * @param path Directory path
 * @return DIR pointer on success, NULL on failure
 *
 * Validates that path is a directory, then attempts to open it.
 * If the filesystem doesn't support directory fds, continues anyway
 * since iteration is path-based.
 */
DIR *hostabi_opendir(const char *path)
{
  hostabi_dir_t *d;
  xv6_kstat_t st;
  int fd;

  path = (const char *)elf_loader_translate_ptr(path);
  if(path == 0){
    errno = EINVAL;
    return 0;
  }
  if(xv6_stat_path(path, &st) != 0){
    errno = xv6_last_errno();
    if(errno <= 0)
      errno = ENOENT;
    return 0;
  }
  if(st.type != XV6_KSTAT_T_DIR){
    errno = ENOTDIR;
    return 0;
  }

  fd = hostabi_posix_fs_open_mode(path, O_RDONLY, 0);
  if(fd < 0){
    fd = -1;
  }

  d = (hostabi_dir_t *)calloc(1, sizeof(*d));
  if(d == 0){
    if(fd >= 0)
      (void)hostabi_posix_fs_close(fd);
    errno = ENOMEM;
    return 0;
  }

  d->magic = HOSTABI_DIR_MAGIC;
  d->fd = fd;
  d->index = 0;
  copy_cstr(d->path, sizeof(d->path), path);
  return (DIR *)d;
}

/**
 * @brief Read next directory entry
 * @param dirp Directory stream
 * @return dirent on success, NULL on EOF or error
 *
 * Calls xv6fs_list_path with the stored path and current index,
 * then increments the index. Returns NULL on EOF (xv6fs_list_path
 * returns non-zero) with errno cleared.
 */
struct dirent *hostabi_readdir(DIR *dirp)
{
  hostabi_dir_t *d = (hostabi_dir_t *)dirp;
  char name[256];
  uint16 type = 0;
  uint32 size = 0;

  if(d == 0 || d->magic != HOSTABI_DIR_MAGIC){
    errno = EBADF;
    return 0;
  }
  if(xv6fs_list_path(d->path, d->index, name, sizeof(name), &type, &size) != 0){
    errno = 0;
    return 0;
  }

  d->index++;
  memset(&d->ent, 0, sizeof(d->ent));
  d->ent.d_ino = (ino_t)d->index;
  d->ent.d_type = (unsigned char)dirent_type_from_xv6(type);
  copy_cstr(d->ent.d_name, (int)sizeof(d->ent.d_name), name);
  return &d->ent;
}

/**
 * @brief Close a directory
 * @param dirp Directory stream
 * @return 0 on success, -1 on failure
 *
 * Validates the handle, clears magic, closes fd if open, and frees memory.
 */
int hostabi_closedir(DIR *dirp)
{
  hostabi_dir_t *d = (hostabi_dir_t *)dirp;
  if(d == 0 || d->magic != HOSTABI_DIR_MAGIC){
    errno = EBADF;
    return -1;
  }
  d->magic = 0;
  if(d->fd >= 0)
    (void)hostabi_posix_fs_close(d->fd);
  free(d);
  return 0;
}

/**
 * @brief Rewind directory to beginning
 * @param dirp Directory stream
 *
 * Resets the index to zero and seeks the fd back to start if present.
 */
void hostabi_rewinddir(DIR *dirp)
{
  hostabi_dir_t *d = (hostabi_dir_t *)dirp;
  if(d == 0 || d->magic != HOSTABI_DIR_MAGIC)
    return;
  d->index = 0;
  if(d->fd >= 0)
    (void)hostabi_posix_fs_lseek(d->fd, 0, 0);
}

/**
 * @brief Get fd for directory stream
 * @param dirp Directory stream
 * @return File descriptor on success, -1 on failure
 *
 * Returns the underlying fd if it was successfully opened.
 */
int hostabi_dirfd(DIR *dirp)
{
  hostabi_dir_t *d = (hostabi_dir_t *)dirp;
  if(d == 0 || d->magic != HOSTABI_DIR_MAGIC){
    errno = EBADF;
    return -1;
  }
  if(d->fd < 0){
    errno = EBADF;
    return -1;
  }
  return d->fd;
}

/**
 * @brief Open directory from fd
 * @param fd File descriptor
 * @return NULL (not implemented)
 *
 * Not implemented - returns NULL with errno = ENOSYS.
 */
DIR *hostabi_fdopendir(int fd)
{
  (void)fd;
  errno = ENOSYS;
  return 0;
}
