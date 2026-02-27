#ifndef XV6_SOCKET_COMPAT_H
#define XV6_SOCKET_COMPAT_H

#include <stdint.h>
#include <stddef.h>

#ifndef AF_INET
#define AF_INET 2
#endif

#ifndef SOCK_STREAM
#define SOCK_STREAM 1
#endif

#ifndef IPPROTO_TCP
#define IPPROTO_TCP 6
#endif

#ifndef SOL_SOCKET
#define SOL_SOCKET 1
#endif

#ifndef SO_REUSEADDR
#define SO_REUSEADDR 2
#endif

#ifndef SO_TYPE
#define SO_TYPE 3
#endif

#ifndef SO_ERROR
#define SO_ERROR 4
#endif

#ifndef SHUT_RD
#define SHUT_RD 0
#endif

#ifndef SHUT_WR
#define SHUT_WR 1
#endif

#ifndef SHUT_RDWR
#define SHUT_RDWR 2
#endif

#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK 0x7f000001u
#endif

#ifndef NETKMOD_IOCTL_FIONBIO
#define NETKMOD_IOCTL_FIONBIO 0x5421UL
#endif

typedef unsigned int socklen_t;

struct in_addr {
  uint32_t s_addr;
};

struct sockaddr {
  unsigned short sa_family;
  char sa_data[14];
};

struct sockaddr_in {
  short sin_family;
  unsigned short sin_port;
  struct in_addr sin_addr;
  unsigned char sin_zero[8];
};

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *addr, socklen_t addrlen);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *addrlen);
int connect(int fd, const struct sockaddr *addr, socklen_t addrlen);
int send(int fd, const void *buf, size_t len, int flags);
int recv(int fd, void *buf, size_t len, int flags);
int shutdown(int fd, int how);
int getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen);
int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen);
int getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen);
int getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen);

#endif
