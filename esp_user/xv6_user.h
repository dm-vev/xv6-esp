#ifndef XV6_USER_H
#define XV6_USER_H

typedef unsigned int u32;
typedef unsigned short u16;

extern int printf(const char *fmt, ...);
extern int usleep(unsigned int usec);

extern unsigned int strlen(const char *s);
extern void *memcpy(void *dst, const void *src, unsigned int n);

extern int k_ticks(void);
extern int k_puts(const char *s);
extern int k_free_heap(void);

extern int xv6fs_readdir_path(const char *path, int index, char *name_out, int name_out_len, u16 *type_out,
                              u32 *size_out);
extern int xv6fs_mkdir_path(const char *path);
extern int xv6fs_unlink_path(const char *path);

extern int xv6_open(const char *path, int flags);
extern int xv6_dup(int fd);
extern int xv6_pipe(int *out_read_fd, int *out_write_fd);
extern int xv6_read(int fd, void *buf, u32 size);
extern int xv6_write(int fd, const void *buf, u32 size);
extern int xv6_close(int fd);
extern int xv6_ptsname(int master_fd, char *out_path, int out_len);

#define XV6_O_RDONLY 0x0000
#define XV6_O_WRONLY 0x0001
#define XV6_O_RDWR   0x0002
#define XV6_O_CREAT  0x0200
#define XV6_O_TRUNC  0x0400
#define XV6_O_APPEND 0x0800

#define O_RDONLY XV6_O_RDONLY
#define O_WRONLY XV6_O_WRONLY
#define O_RDWR   XV6_O_RDWR
#define O_CREAT  XV6_O_CREAT
#define O_TRUNC  XV6_O_TRUNC
#define O_APPEND XV6_O_APPEND

#endif
