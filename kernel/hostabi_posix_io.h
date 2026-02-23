#ifndef XV6_HOSTABI_POSIX_IO_H
#define XV6_HOSTABI_POSIX_IO_H

#include <termios.h>

void hostabi_posix_io_init(void);
void hostabi_posix_io_on_open(int fd, int flags, const char *path);
void hostabi_posix_io_on_close(int fd, int keep_stdio_defaults);
void hostabi_posix_io_on_dup(int oldfd, int newfd);

int hostabi_posix_isatty(int fd);
int hostabi_posix_fcntl(int fd, int cmd, ...);
int hostabi_posix_ioctl(int fd, unsigned long request, ...);
int hostabi_posix_tcgetattr(int fd, struct termios *tio);
int hostabi_posix_tcsetattr(int fd, int optional_actions, const struct termios *tio);
void hostabi_posix_cfmakeraw(struct termios *tio);

#endif
