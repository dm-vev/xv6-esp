#ifndef XV6_HOSTABI_PTY_H
#define XV6_HOSTABI_PTY_H

#include <stddef.h>

int hostabi_posix_openpt(int flags);
int hostabi_grantpt(int fd);
int hostabi_unlockpt(int fd);
char *hostabi_ptsname(int fd);
int hostabi_ptsname_r(int fd, char *buf, size_t buflen);

#endif
