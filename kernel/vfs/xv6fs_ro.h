#ifndef XV6_XV6FS_RO_H
#define XV6_XV6FS_RO_H

#include "core/types.h"

// Global runtime capacity knobs for the lightweight VFS layer.
// Keep these in sync with shell/runtime limits.
#define XV6_TASK_CTX_CAP 64
#define XV6_FD_CAP       128
#define XV6_PTY_CAP      8
#define XV6_PIPE_CAP     64

int xv6fs_ro_init(void);
int xv6fs_ro_flash_image(const uint8 *image, uint32 image_size);
int xv6fs_ro_list(int index, char *name_out, int name_out_len, uint32 *size_out);
int xv6fs_ro_read_file_alloc(const char *name, void **out_data, uint32 *out_size);
int xv6fs_read_file_alloc_path(const char *path, void **out_data, uint32 *out_size);
int xv6fs_list_path(const char *path, int index, char *name_out, int name_out_len, uint16 *type_out,
                    uint32 *size_out);
int xv6fs_write_file_path(const char *path, const void *data, uint32 size);
int xv6fs_mkdir_path(const char *path);
int xv6fs_unlink_path(const char *path);
int xv6fs_rmdir_path(const char *path);
int xv6fs_rename_path(const char *oldpath, const char *newpath);

// Tiny VFS-like FD API used by ELF usermode commands.
int xv6_open(const char *path, int flags);
int xv6_dup(int fd);
int xv6_read(int fd, void *buf, uint32 size);
int xv6_write(int fd, const void *buf, uint32 size);
int xv6_close(int fd);
int xv6_lseek(int fd, int offset, int whence);
int xv6_set_status_flags(int fd, int status_flags);
int xv6_chdir(const char *path);
int xv6_getcwd(char *out_path, int out_len);
int xv6_ptsname(int master_fd, char *out_path, int out_len);
int xv6_pipe(int *out_read_fd, int *out_write_fd);
int xv6_access(const char *path, int mode);
int xv6_chmod(const char *path, int mode);

typedef struct {
  uint32 ino;
  uint32 size;
  uint16 type;
  uint16 nlink;
} xv6_kstat_t;

int xv6_stat_path(const char *path, xv6_kstat_t *st);
int xv6_fstat(int fd, xv6_kstat_t *st);
int xv6_last_errno(void);
int xv6_stdio_set_fds(int in_fd, int out_fd, int err_fd);
void xv6_stdio_reset_fds(void);
int xv6_stdio_is_default_out(void);
void xv6_task_ctx_cleanup(void);
void xv6_task_ctx_cleanup_for_handle(void *task_handle);
void xv6_vfs_reset(void);

#define XV6_O_RDONLY 0x0000
#define XV6_O_WRONLY 0x0001
#define XV6_O_RDWR   0x0002
#define XV6_O_ACCMODE 0x0003
#define XV6_O_CREAT  0x0200
#define XV6_O_TRUNC  0x0400
#define XV6_O_APPEND 0x0800

#endif
