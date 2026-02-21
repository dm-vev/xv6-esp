#ifndef XV6_XV6FS_RO_H
#define XV6_XV6FS_RO_H

#include "types.h"

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

// Tiny VFS-like FD API used by ELF usermode commands.
int xv6_open(const char *path, int flags);
int xv6_read(int fd, void *buf, uint32 size);
int xv6_write(int fd, const void *buf, uint32 size);
int xv6_close(int fd);
int xv6_ptsname(int master_fd, char *out_path, int out_len);
void xv6_vfs_reset(void);

#define XV6_O_RDONLY 0x0000
#define XV6_O_WRONLY 0x0001
#define XV6_O_RDWR   0x0002
#define XV6_O_CREAT  0x0200
#define XV6_O_TRUNC  0x0400
#define XV6_O_APPEND 0x0800

#endif
