#ifndef XV6_SOCKET_COMPAT_H
#define XV6_SOCKET_COMPAT_H

/*
 * netkmod exposes virtual socket descriptors above the VFS fd range, so
 * fd_set users need a larger set than newlib's small embedded default.
 */
#ifndef FD_SETSIZE
#define FD_SETSIZE 512
#endif

#include <stdint.h>
#include <stddef.h>
#include <sys/time.h>

#ifndef AF_INET
#define AF_INET 2
#endif

#ifndef AF_INET6
#define AF_INET6 10
#endif

#ifndef SOCK_STREAM
#define SOCK_STREAM 1
#endif

#ifndef SOCK_DGRAM
#define SOCK_DGRAM 2
#endif

#ifndef IPPROTO_TCP
#define IPPROTO_TCP 6
#endif

#ifndef IPPROTO_UDP
#define IPPROTO_UDP 17
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

/*
 * Keep socket ABI values aligned with ESP-IDF's newlib platform headers.
 * Applets are built freestanding, so they cannot include sys/poll.h directly.
 */
#ifndef NETKMOD_IOCTL_FIONREAD
#define NETKMOD_IOCTL_FIONREAD 0x4004667fUL
#endif

#ifndef NETKMOD_IOCTL_FIONBIO
#define NETKMOD_IOCTL_FIONBIO 0x8004667eUL
#endif

typedef unsigned int socklen_t;
typedef unsigned int nfds_t;
typedef uint8_t sa_family_t;

struct in_addr {
  uint32_t s_addr;
};

struct in6_addr {
  unsigned char s6_addr[16];
};

struct sockaddr {
  uint8_t sa_len;
  sa_family_t sa_family;
  char sa_data[14];
};

struct sockaddr_in {
  uint8_t sin_len;
  sa_family_t sin_family;
  unsigned short sin_port;
  struct in_addr sin_addr;
  unsigned char sin_zero[8];
};

struct sockaddr_in6 {
  uint8_t sin6_len;
  sa_family_t sin6_family;
  unsigned short sin6_port;
  uint32_t sin6_flowinfo;
  struct in6_addr sin6_addr;
  uint32_t sin6_scope_id;
};

#ifndef POLLIN
#define POLLIN 0x0001
#endif
#ifndef POLLOUT
#define POLLOUT 0x0008
#endif
#ifndef POLLERR
#define POLLERR 0x0020
#endif
#ifndef POLLNVAL
#define POLLNVAL 0x0080
#endif
#ifndef POLLHUP
#define POLLHUP 0x0040
#endif

struct pollfd {
  int fd;
  short events;
  short revents;
};

#ifndef FD_SETSIZE
#define FD_SETSIZE 512
typedef struct {
  unsigned long fds_bits[(FD_SETSIZE + (8 * (int)sizeof(unsigned long) - 1)) / (8 * (int)sizeof(unsigned long))];
} fd_set;
#define __XV6_FD_BITS_PER_WORD ((int)(8 * sizeof(unsigned long)))
#define __XV6_FD_WORD(fd) ((fd) / __XV6_FD_BITS_PER_WORD)
#define __XV6_FD_MASK(fd) (1ul << ((fd) % __XV6_FD_BITS_PER_WORD))
#define FD_ZERO(setp)                                                                                                  \
  do {                                                                                                                 \
    int __i;                                                                                                           \
    for(__i = 0; __i < (int)(sizeof((setp)->fds_bits) / sizeof((setp)->fds_bits[0])); __i++)                         \
      (setp)->fds_bits[__i] = 0ul;                                                                                     \
  } while(0)
#define FD_SET(fd, setp)                                                                                                \
  do {                                                                                                                  \
    if((fd) >= 0 && (fd) < FD_SETSIZE)                                                                                 \
      (setp)->fds_bits[__XV6_FD_WORD(fd)] |= __XV6_FD_MASK(fd);                                                        \
  } while(0)
#define FD_CLR(fd, setp)                                                                                                \
  do {                                                                                                                  \
    if((fd) >= 0 && (fd) < FD_SETSIZE)                                                                                 \
      (setp)->fds_bits[__XV6_FD_WORD(fd)] &= ~__XV6_FD_MASK(fd);                                                       \
  } while(0)
#define FD_ISSET(fd, setp)                                                                                              \
  (((fd) >= 0 && (fd) < FD_SETSIZE) ? (((setp)->fds_bits[__XV6_FD_WORD(fd)] & __XV6_FD_MASK(fd)) != 0ul) : 0)
#endif

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *addr, socklen_t addrlen);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *addrlen);
int connect(int fd, const struct sockaddr *addr, socklen_t addrlen);
int send(int fd, const void *buf, size_t len, int flags);
int recv(int fd, void *buf, size_t len, int flags);
int sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *addr, socklen_t addrlen);
int recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *addr, socklen_t *addrlen);
int poll(struct pollfd *fds, nfds_t nfds, int timeout);
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
int shutdown(int fd, int how);
int getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen);
int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen);
int getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen);
int getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen);

#endif
