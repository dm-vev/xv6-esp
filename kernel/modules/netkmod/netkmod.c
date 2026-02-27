#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xv6_module.h"
#include "xv6_socket_compat.h"

#define NETKMOD_MAX_SOCK 64
#define NETKMOD_FD_BASE 200

typedef struct {
  int used;
  int host_fd;
} net_socket_t;

typedef struct {
  uint32_t sockets_created;
  uint32_t sockets_closed;
  uint32_t connect_ok;
  uint32_t connect_fail;
  uint32_t accept_ok;
  uint32_t tx_packets;
  uint32_t rx_packets;
  uint32_t tx_bytes;
  uint32_t rx_bytes;
  uint32_t drops;
} net_stats_t;

typedef struct {
  uint32_t sockets_created;
  uint32_t sockets_closed;
  uint32_t connect_ok;
  uint32_t connect_fail;
  uint32_t accept_ok;
  uint32_t tx_packets;
  uint32_t rx_packets;
  uint32_t tx_bytes;
  uint32_t rx_bytes;
  uint32_t drops;
  uint32_t active_sockets;
} netkmod_stats_export_t;

extern int __xv6_host_close(int fd);
extern int __xv6_host_fcntl(int fd, int cmd, ...);
extern int __xv6_host_ioctl(int fd, unsigned long request, ...);

extern int __xv6_posix_close(int fd);
extern int __xv6_posix_fcntl(int fd, int cmd, ...);
extern int __xv6_posix_ioctl(int fd, unsigned long request, ...);
extern int __xv6_posix_socket(int domain, int type, int protocol);
extern int __xv6_posix_bind(int fd, const struct sockaddr *addr, socklen_t addrlen);
extern int __xv6_posix_listen(int fd, int backlog);
extern int __xv6_posix_accept(int fd, struct sockaddr *addr, socklen_t *addrlen);
extern int __xv6_posix_connect(int fd, const struct sockaddr *addr, socklen_t addrlen);
extern int __xv6_posix_send(int fd, const void *buf, size_t len, int flags);
extern int __xv6_posix_recv(int fd, void *buf, size_t len, int flags);
extern int __xv6_posix_shutdown(int fd, int how);
extern int __xv6_posix_getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen);
extern int __xv6_posix_setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen);
extern int __xv6_posix_getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen);
extern int __xv6_posix_getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen);

static volatile int g_lock;
static net_socket_t g_socks[NETKMOD_MAX_SOCK];
static net_stats_t g_stats;
static int g_trace_verbose = 1;

static void net_lock(void)
{
  while(__sync_lock_test_and_set(&g_lock, 1) != 0)
    ;
}

static void net_unlock(void)
{
  __sync_lock_release(&g_lock);
}

static int slot_to_fd(int slot)
{
  return NETKMOD_FD_BASE + slot;
}

static int slot_from_fd_locked(int fd)
{
  int slot = fd - NETKMOD_FD_BASE;
  if(slot < 0 || slot >= NETKMOD_MAX_SOCK)
    return -1;
  if(!g_socks[slot].used)
    return -1;
  return slot;
}

static uint32_t active_sockets_locked(void)
{
  uint32_t n = 0;
  int i;
  for(i = 0; i < NETKMOD_MAX_SOCK; i++){
    if(g_socks[i].used)
      n++;
  }
  return n;
}

static void tracef(const char *event, int fd, uint32_t v)
{
  if(!g_trace_verbose)
    return;
  printf("netkmod: event=%s fd=%d val=%u\n", event, fd, (unsigned)v);
}

static int alloc_slot_locked(int host_fd)
{
  int i;
  for(i = 0; i < NETKMOD_MAX_SOCK; i++){
    if(!g_socks[i].used){
      g_socks[i].used = 1;
      g_socks[i].host_fd = host_fd;
      g_stats.sockets_created++;
      return i;
    }
  }
  return -1;
}

static void free_slot_locked(int slot)
{
  if(slot < 0 || slot >= NETKMOD_MAX_SOCK || !g_socks[slot].used)
    return;
  memset(&g_socks[slot], 0, sizeof(g_socks[slot]));
  g_stats.sockets_closed++;
}

static int fcntl_cmd_needs_arg(int cmd)
{
  switch(cmd){
#ifdef F_DUPFD
  case F_DUPFD:
#endif
#ifdef F_DUPFD_CLOEXEC
  case F_DUPFD_CLOEXEC:
#endif
#ifdef F_SETFD
  case F_SETFD:
#endif
#ifdef F_SETFL
  case F_SETFL:
#endif
    return 1;
  default:
    return 0;
  }
}

static int ioctl_cmd_needs_arg(unsigned long req)
{
  if(req == NETKMOD_IOCTL_FIONBIO)
    return 1;
  return 0;
}

int netkmod_socket(int domain, int type, int protocol)
{
  int host_fd;
  int slot;

  if(domain != AF_INET){
    errno = EAFNOSUPPORT;
    return -1;
  }
  if((type & 0xff) != SOCK_STREAM){
    errno = EPROTONOSUPPORT;
    return -1;
  }
  if(protocol != 0 && protocol != IPPROTO_TCP){
    errno = EPROTONOSUPPORT;
    return -1;
  }

  host_fd = __xv6_posix_socket(domain, type, protocol);
  if(host_fd < 0)
    return -1;

  net_lock();
  slot = alloc_slot_locked(host_fd);
  if(slot < 0){
    g_stats.drops++;
    net_unlock();
    (void)__xv6_posix_close(host_fd);
    errno = EMFILE;
    return -1;
  }
  net_unlock();

  tracef("socket", slot_to_fd(slot), (uint32_t)host_fd);
  return slot_to_fd(slot);
}

int netkmod_bind(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
  int slot;
  int rc;

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    errno = EBADF;
    return -1;
  }
  rc = __xv6_posix_bind(g_socks[slot].host_fd, addr, addrlen);
  net_unlock();
  return rc;
}

int netkmod_listen(int fd, int backlog)
{
  int slot;
  int rc;

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    errno = EBADF;
    return -1;
  }
  rc = __xv6_posix_listen(g_socks[slot].host_fd, backlog);
  net_unlock();
  return rc;
}

int netkmod_connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
  int slot;
  int rc;

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    errno = EBADF;
    return -1;
  }
  rc = __xv6_posix_connect(g_socks[slot].host_fd, addr, addrlen);
  if(rc == 0)
    g_stats.connect_ok++;
  else
    g_stats.connect_fail++;
  net_unlock();

  return rc;
}

int netkmod_accept(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
  int slot;
  int host_fd;
  int child_slot;

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    errno = EBADF;
    return -1;
  }

  host_fd = __xv6_posix_accept(g_socks[slot].host_fd, addr, addrlen);
  if(host_fd < 0){
    net_unlock();
    return -1;
  }

  child_slot = alloc_slot_locked(host_fd);
  if(child_slot < 0){
    g_stats.drops++;
    net_unlock();
    (void)__xv6_posix_close(host_fd);
    errno = EMFILE;
    return -1;
  }

  g_stats.accept_ok++;
  net_unlock();

  tracef("accept", slot_to_fd(child_slot), (uint32_t)host_fd);
  return slot_to_fd(child_slot);
}

int netkmod_send(int fd, const void *buf, size_t len, int flags)
{
  int slot;
  int rc;

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    errno = EBADF;
    return -1;
  }
  rc = __xv6_posix_send(g_socks[slot].host_fd, buf, len, flags);
  if(rc > 0){
    g_stats.tx_packets++;
    g_stats.tx_bytes += (uint32_t)rc;
  }
  net_unlock();

  return rc;
}

int netkmod_recv(int fd, void *buf, size_t len, int flags)
{
  int slot;
  int rc;

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    errno = EBADF;
    return -1;
  }
  rc = __xv6_posix_recv(g_socks[slot].host_fd, buf, len, flags);
  if(rc > 0){
    g_stats.rx_packets++;
    g_stats.rx_bytes += (uint32_t)rc;
  }
  net_unlock();

  return rc;
}

int netkmod_shutdown(int fd, int how)
{
  int slot;
  int rc;

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    errno = EBADF;
    return -1;
  }
  rc = __xv6_posix_shutdown(g_socks[slot].host_fd, how);
  net_unlock();

  return rc;
}

int netkmod_getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen)
{
  int slot;
  int rc;

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    errno = ENOTSOCK;
    return -1;
  }
  rc = __xv6_posix_getsockopt(g_socks[slot].host_fd, level, optname, optval, optlen);
  net_unlock();

  return rc;
}

int netkmod_setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen)
{
  int slot;
  int rc;

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    errno = ENOTSOCK;
    return -1;
  }
  rc = __xv6_posix_setsockopt(g_socks[slot].host_fd, level, optname, optval, optlen);
  net_unlock();

  return rc;
}

int netkmod_getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
  int slot;
  int rc;

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    errno = ENOTSOCK;
    return -1;
  }
  rc = __xv6_posix_getsockname(g_socks[slot].host_fd, addr, addrlen);
  net_unlock();

  return rc;
}

int netkmod_getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
  int slot;
  int rc;

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    errno = ENOTSOCK;
    return -1;
  }
  rc = __xv6_posix_getpeername(g_socks[slot].host_fd, addr, addrlen);
  net_unlock();

  return rc;
}

int netkmod_close(int fd)
{
  int slot;

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    return __xv6_host_close(fd);
  }

  if(__xv6_posix_close(g_socks[slot].host_fd) != 0){
    if(errno != EBADF){
      net_unlock();
      return -1;
    }
  }
  free_slot_locked(slot);
  net_unlock();

  tracef("close", fd, 0);
  return 0;
}

int netkmod_fcntl(int fd, int cmd, ...)
{
  va_list ap;
  int arg = 0;
  int slot;
  int rc;

  if(fcntl_cmd_needs_arg(cmd)){
    va_start(ap, cmd);
    arg = va_arg(ap, int);
    va_end(ap);
  }

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    if(fcntl_cmd_needs_arg(cmd))
      return __xv6_host_fcntl(fd, cmd, arg);
    return __xv6_host_fcntl(fd, cmd);
  }

  if(fcntl_cmd_needs_arg(cmd))
    rc = __xv6_posix_fcntl(g_socks[slot].host_fd, cmd, arg);
  else
    rc = __xv6_posix_fcntl(g_socks[slot].host_fd, cmd);
  net_unlock();

  return rc;
}

int netkmod_ioctl(int fd, unsigned long request, ...)
{
  va_list ap;
  void *arg = 0;
  int slot;
  int rc;

  if(ioctl_cmd_needs_arg(request)){
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);
  }

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    if(ioctl_cmd_needs_arg(request))
      return __xv6_host_ioctl(fd, request, arg);
    return __xv6_host_ioctl(fd, request);
  }

  if(ioctl_cmd_needs_arg(request))
    rc = __xv6_posix_ioctl(g_socks[slot].host_fd, request, arg);
  else
    rc = __xv6_posix_ioctl(g_socks[slot].host_fd, request);
  net_unlock();

  return rc;
}

int netkmod_get_stats(netkmod_stats_export_t *out, uint32_t out_size)
{
  netkmod_stats_export_t tmp;

  if(out == 0 || out_size < sizeof(tmp)){
    errno = EINVAL;
    return -1;
  }

  net_lock();
  memset(&tmp, 0, sizeof(tmp));
  tmp.sockets_created = g_stats.sockets_created;
  tmp.sockets_closed = g_stats.sockets_closed;
  tmp.connect_ok = g_stats.connect_ok;
  tmp.connect_fail = g_stats.connect_fail;
  tmp.accept_ok = g_stats.accept_ok;
  tmp.tx_packets = g_stats.tx_packets;
  tmp.rx_packets = g_stats.rx_packets;
  tmp.tx_bytes = g_stats.tx_bytes;
  tmp.rx_bytes = g_stats.rx_bytes;
  tmp.drops = g_stats.drops;
  tmp.active_sockets = active_sockets_locked();
  net_unlock();

  memcpy(out, &tmp, sizeof(tmp));
  return 0;
}

int netkmod_set_trace(int enabled)
{
  g_trace_verbose = enabled ? 1 : 0;
  return 0;
}

int xv6_module_init(void)
{
  net_lock();
  memset(g_socks, 0, sizeof(g_socks));
  memset(&g_stats, 0, sizeof(g_stats));
  net_unlock();
  tracef("init", -1, 0);
  return 0;
}

int xv6_module_fini(void)
{
  net_lock();
  memset(g_socks, 0, sizeof(g_socks));
  net_unlock();
  tracef("fini", -1, 0);
  return 0;
}

static const xv6_module_symbol_t g_symbols[] = {
  { "socket", (void *)netkmod_socket, XV6_MODULE_SYMBOL_OVERRIDE, 50 },
  { "bind", (void *)netkmod_bind, XV6_MODULE_SYMBOL_OVERRIDE, 50 },
  { "listen", (void *)netkmod_listen, XV6_MODULE_SYMBOL_OVERRIDE, 50 },
  { "accept", (void *)netkmod_accept, XV6_MODULE_SYMBOL_OVERRIDE, 50 },
  { "connect", (void *)netkmod_connect, XV6_MODULE_SYMBOL_OVERRIDE, 50 },
  { "send", (void *)netkmod_send, XV6_MODULE_SYMBOL_OVERRIDE, 50 },
  { "recv", (void *)netkmod_recv, XV6_MODULE_SYMBOL_OVERRIDE, 50 },
  { "shutdown", (void *)netkmod_shutdown, XV6_MODULE_SYMBOL_OVERRIDE, 50 },
  { "getsockopt", (void *)netkmod_getsockopt, XV6_MODULE_SYMBOL_OVERRIDE, 50 },
  { "setsockopt", (void *)netkmod_setsockopt, XV6_MODULE_SYMBOL_OVERRIDE, 50 },
  { "getsockname", (void *)netkmod_getsockname, XV6_MODULE_SYMBOL_OVERRIDE, 50 },
  { "getpeername", (void *)netkmod_getpeername, XV6_MODULE_SYMBOL_OVERRIDE, 50 },
  { "close", (void *)netkmod_close, XV6_MODULE_SYMBOL_OVERRIDE, 50 },
  { "fcntl", (void *)netkmod_fcntl, XV6_MODULE_SYMBOL_OVERRIDE, 50 },
  { "ioctl", (void *)netkmod_ioctl, XV6_MODULE_SYMBOL_OVERRIDE, 50 },
  { "netkmod_get_stats", (void *)netkmod_get_stats, XV6_MODULE_SYMBOL_EXTENSION, 50 },
  { "netkmod_set_trace", (void *)netkmod_set_trace, XV6_MODULE_SYMBOL_EXTENSION, 50 },
};

static const xv6_module_desc_t g_desc = {
  KMOD_MODULE_ABI_VER,
  "netkmod",
  50,
  g_symbols,
  (int)(sizeof(g_symbols) / sizeof(g_symbols[0])),
};

const xv6_module_desc_t *xv6_module_describe(void)
{
  return &g_desc;
}
