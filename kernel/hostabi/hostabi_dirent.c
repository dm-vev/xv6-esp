#include "hostabi/hostabi_dirent.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "loader/elf_loader.h"
#include "hostabi/hostabi_posix_fs.h"
#include "core/param.h"
#include "vfs/xv6fs_ro.h"

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

#define XV6_KSTAT_T_DIR 1
#define XV6_KSTAT_T_FILE 2
#define XV6_KSTAT_T_DEVICE 3
#define HOSTABI_DIR_MAGIC 0x48445231u

typedef struct {
  uint32 magic;
  int fd;
  int index;
  char path[MAXPATH];
  struct dirent ent;
} hostabi_dir_t;

static void copy_cstr(char *dst, int dst_len, const char *src)
{
  if(dst == 0 || dst_len <= 0)
    return;
  if(src == 0)
    src = "";
  strncpy(dst, src, (size_t)dst_len - 1u);
  dst[dst_len - 1] = 0;
}

static int dirent_type_from_xv6(uint16 type)
{
  if(type == XV6_KSTAT_T_DIR)
    return DT_DIR;
  if(type == XV6_KSTAT_T_FILE)
    return DT_REG;
  if(type == XV6_KSTAT_T_DEVICE)
    return DT_CHR;
  return DT_UNKNOWN;
}

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
    /*
     * Directory iteration is path-based (xv6fs_list_path) and does not
     * require a live fd. Keep opendir() functional even when the backend
     * cannot open directory fds.
     */
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

void hostabi_rewinddir(DIR *dirp)
{
  hostabi_dir_t *d = (hostabi_dir_t *)dirp;
  if(d == 0 || d->magic != HOSTABI_DIR_MAGIC)
    return;
  d->index = 0;
  if(d->fd >= 0)
    (void)hostabi_posix_fs_lseek(d->fd, 0, 0 /* SEEK_SET */);
}

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

DIR *hostabi_fdopendir(int fd)
{
  (void)fd;
  errno = ENOSYS;
  return 0;
}
