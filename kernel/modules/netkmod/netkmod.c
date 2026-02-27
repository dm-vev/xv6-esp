#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "xv6_module.h"

#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK 0x7f000001UL
#endif

#ifndef SO_TYPE
#define SO_TYPE 3
#endif

#ifndef SO_ERROR
#define SO_ERROR 4
#endif

#ifndef SO_REUSEADDR
#define SO_REUSEADDR 2
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define NETKMOD_MAX_SOCK 32
#define NETKMOD_FD_BASE 200
#define NETKMOD_RX_CAP 4096
#define NETKMOD_MAX_PENDING 16
#define NETKMOD_EPHEMERAL_MIN 41000u
#define NETKMOD_EPHEMERAL_MAX 48999u

typedef enum {
  NET_STATE_FREE = 0,
  NET_STATE_CREATED,
  NET_STATE_BOUND,
  NET_STATE_LISTEN,
  NET_STATE_ESTABLISHED,
} net_state_t;

typedef struct {
  int used;
  int nonblock;
  net_state_t state;
  int backlog;
  uint16_t local_port;
  uint16_t peer_port;
  int peer_slot;
  int peer_eof;
  int shut_rd;
  int shut_wr;

  int pending[NETKMOD_MAX_PENDING];
  int pending_head;
  int pending_tail;
  int pending_count;

  uint8_t rx[NETKMOD_RX_CAP];
  uint16_t rx_r;
  uint16_t rx_w;
  uint16_t rx_n;
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

static volatile int g_lock;
static net_socket_t g_socks[NETKMOD_MAX_SOCK];
static net_stats_t g_stats;
static uint16_t g_next_ephemeral = NETKMOD_EPHEMERAL_MIN;
static int g_trace_verbose = 1;

static void net_lock(void)
{
  while(__sync_lock_test_and_set(&g_lock, 1) != 0)
    usleep(1000);
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

static int is_socket_fd_locked(int fd)
{
  return slot_from_fd_locked(fd) >= 0;
}

static uint32_t count_active_sockets_locked(void)
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

static int alloc_slot_locked(void)
{
  int i;
  for(i = 0; i < NETKMOD_MAX_SOCK; i++){
    if(!g_socks[i].used){
      memset(&g_socks[i], 0, sizeof(g_socks[i]));
      g_socks[i].used = 1;
      g_socks[i].state = NET_STATE_CREATED;
      g_socks[i].peer_slot = -1;
      g_socks[i].backlog = 1;
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

static void mark_peer_eof_locked(int slot)
{
  int peer;
  if(slot < 0 || slot >= NETKMOD_MAX_SOCK || !g_socks[slot].used)
    return;
  peer = g_socks[slot].peer_slot;
  if(peer >= 0 && peer < NETKMOD_MAX_SOCK && g_socks[peer].used){
    g_socks[peer].peer_eof = 1;
    g_socks[peer].peer_slot = -1;
  }
  g_socks[slot].peer_slot = -1;
}

static int port_in_use_locked(uint16_t port, int except_slot)
{
  int i;
  if(port == 0)
    return 0;
  for(i = 0; i < NETKMOD_MAX_SOCK; i++){
    if(i == except_slot)
      continue;
    if(!g_socks[i].used)
      continue;
    if(g_socks[i].local_port == port)
      return 1;
  }
  return 0;
}

static uint16_t alloc_ephemeral_port_locked(int except_slot)
{
  uint16_t probe;
  uint32_t tries;

  probe = g_next_ephemeral;
  for(tries = 0; tries <= (NETKMOD_EPHEMERAL_MAX - NETKMOD_EPHEMERAL_MIN); tries++){
    if(probe < NETKMOD_EPHEMERAL_MIN || probe > NETKMOD_EPHEMERAL_MAX)
      probe = NETKMOD_EPHEMERAL_MIN;
    if(!port_in_use_locked(probe, except_slot)){
      g_next_ephemeral = (probe == NETKMOD_EPHEMERAL_MAX) ? NETKMOD_EPHEMERAL_MIN : (uint16_t)(probe + 1u);
      return probe;
    }
    probe = (probe == NETKMOD_EPHEMERAL_MAX) ? NETKMOD_EPHEMERAL_MIN : (uint16_t)(probe + 1u);
  }

  return 0;
}

static int parse_sockaddr_port(const struct sockaddr *addr, socklen_t addrlen, uint16_t *port_out)
{
  const struct sockaddr_in *in;

  if(addr == 0 || port_out == 0){
    errno = EINVAL;
    return -1;
  }
  if(addrlen < (socklen_t)sizeof(struct sockaddr_in)){
    errno = EINVAL;
    return -1;
  }

  in = (const struct sockaddr_in *)addr;
  if(in->sin_family != AF_INET){
    errno = EAFNOSUPPORT;
    return -1;
  }

  *port_out = ntohs(in->sin_port);
  return 0;
}

static int fill_sockaddr(uint16_t port, struct sockaddr *addr, socklen_t *addrlen)
{
  struct sockaddr_in out;

  if(addr == 0 || addrlen == 0){
    errno = EINVAL;
    return -1;
  }
  if(*addrlen < (socklen_t)sizeof(out)){
    errno = EINVAL;
    return -1;
  }

  memset(&out, 0, sizeof(out));
  out.sin_family = AF_INET;
  out.sin_port = htons(port);
  out.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  memcpy(addr, &out, sizeof(out));
  *addrlen = (socklen_t)sizeof(out);
  return 0;
}

static int find_listener_by_port_locked(uint16_t port)
{
  int i;
  if(port == 0)
    return -1;
  for(i = 0; i < NETKMOD_MAX_SOCK; i++){
    if(!g_socks[i].used)
      continue;
    if(g_socks[i].state != NET_STATE_LISTEN)
      continue;
    if(g_socks[i].local_port == port)
      return i;
  }
  return -1;
}

static int pending_push_locked(int listener_slot, int child_slot)
{
  net_socket_t *ls;
  int limit;

  if(listener_slot < 0 || listener_slot >= NETKMOD_MAX_SOCK || child_slot < 0 || child_slot >= NETKMOD_MAX_SOCK)
    return -1;

  ls = &g_socks[listener_slot];
  if(!ls->used || ls->state != NET_STATE_LISTEN)
    return -1;

  limit = ls->backlog;
  if(limit < 1)
    limit = 1;
  if(limit > NETKMOD_MAX_PENDING)
    limit = NETKMOD_MAX_PENDING;

  if(ls->pending_count >= limit)
    return -1;

  ls->pending[ls->pending_tail] = child_slot;
  ls->pending_tail = (ls->pending_tail + 1) % NETKMOD_MAX_PENDING;
  ls->pending_count++;
  return 0;
}

static int pending_pop_locked(int listener_slot)
{
  net_socket_t *ls;
  int slot;

  if(listener_slot < 0 || listener_slot >= NETKMOD_MAX_SOCK)
    return -1;

  ls = &g_socks[listener_slot];
  if(!ls->used || ls->state != NET_STATE_LISTEN)
    return -1;

  if(ls->pending_count <= 0)
    return -1;

  slot = ls->pending[ls->pending_head];
  ls->pending_head = (ls->pending_head + 1) % NETKMOD_MAX_PENDING;
  ls->pending_count--;
  return slot;
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
#ifdef FIONBIO
  if(req == (unsigned long)FIONBIO)
    return 1;
#endif
#ifdef TIOCGWINSZ
  if(req == (unsigned long)TIOCGWINSZ)
    return 1;
#endif
#ifdef TIOCSWINSZ
  if(req == (unsigned long)TIOCSWINSZ)
    return 1;
#endif
#ifdef TCGETS
  if(req == (unsigned long)TCGETS)
    return 1;
#endif
#ifdef TCSETS
  if(req == (unsigned long)TCSETS)
    return 1;
#endif
#ifdef TCSETSW
  if(req == (unsigned long)TCSETSW)
    return 1;
#endif
#ifdef TCSETSF
  if(req == (unsigned long)TCSETSF)
    return 1;
#endif
  return 0;
}

int netkmod_socket(int domain, int type, int protocol)
{
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

  net_lock();
  slot = alloc_slot_locked();
  if(slot < 0){
    net_unlock();
    errno = EMFILE;
    return -1;
  }
  net_unlock();

  tracef("socket", slot_to_fd(slot), 0);
  return slot_to_fd(slot);
}

int netkmod_bind(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
  int slot;
  uint16_t port;

  if(parse_sockaddr_port(addr, addrlen, &port) != 0)
    return -1;

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    errno = EBADF;
    return -1;
  }
  if(g_socks[slot].state != NET_STATE_CREATED && g_socks[slot].state != NET_STATE_BOUND){
    net_unlock();
    errno = EINVAL;
    return -1;
  }

  if(port == 0){
    port = alloc_ephemeral_port_locked(slot);
    if(port == 0){
      net_unlock();
      errno = EADDRINUSE;
      return -1;
    }
  } else if(port_in_use_locked(port, slot)){
    net_unlock();
    errno = EADDRINUSE;
    return -1;
  }

  g_socks[slot].local_port = port;
  g_socks[slot].state = NET_STATE_BOUND;
  net_unlock();

  tracef("bind", fd, port);
  return 0;
}

int netkmod_listen(int fd, int backlog)
{
  int slot;

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    errno = EBADF;
    return -1;
  }
  if(g_socks[slot].state != NET_STATE_CREATED && g_socks[slot].state != NET_STATE_BOUND){
    net_unlock();
    errno = EINVAL;
    return -1;
  }

  if(g_socks[slot].local_port == 0){
    g_socks[slot].local_port = alloc_ephemeral_port_locked(slot);
    if(g_socks[slot].local_port == 0){
      net_unlock();
      errno = EADDRINUSE;
      return -1;
    }
  }

  if(backlog <= 0)
    backlog = 1;
  if(backlog > NETKMOD_MAX_PENDING)
    backlog = NETKMOD_MAX_PENDING;

  g_socks[slot].backlog = backlog;
  g_socks[slot].state = NET_STATE_LISTEN;
  net_unlock();

  tracef("listen", fd, (uint32_t)backlog);
  return 0;
}

int netkmod_connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
  int client_slot;
  int listener_slot;
  int server_slot;
  uint16_t remote_port;

  if(parse_sockaddr_port(addr, addrlen, &remote_port) != 0)
    return -1;

  net_lock();
  client_slot = slot_from_fd_locked(fd);
  if(client_slot < 0){
    net_unlock();
    errno = EBADF;
    return -1;
  }
  if(g_socks[client_slot].state != NET_STATE_CREATED && g_socks[client_slot].state != NET_STATE_BOUND){
    net_unlock();
    errno = EISCONN;
    return -1;
  }

  listener_slot = find_listener_by_port_locked(remote_port);
  if(listener_slot < 0){
    g_stats.connect_fail++;
    net_unlock();
    errno = ECONNREFUSED;
    return -1;
  }

  server_slot = alloc_slot_locked();
  if(server_slot < 0){
    g_stats.connect_fail++;
    net_unlock();
    errno = ENFILE;
    return -1;
  }

  if(g_socks[client_slot].local_port == 0){
    g_socks[client_slot].local_port = alloc_ephemeral_port_locked(client_slot);
    if(g_socks[client_slot].local_port == 0){
      free_slot_locked(server_slot);
      g_stats.connect_fail++;
      net_unlock();
      errno = EADDRINUSE;
      return -1;
    }
  }

  g_socks[client_slot].peer_slot = server_slot;
  g_socks[client_slot].peer_port = remote_port;
  g_socks[client_slot].peer_eof = 0;
  g_socks[client_slot].state = NET_STATE_ESTABLISHED;

  g_socks[server_slot].state = NET_STATE_ESTABLISHED;
  g_socks[server_slot].local_port = g_socks[listener_slot].local_port;
  g_socks[server_slot].peer_port = g_socks[client_slot].local_port;
  g_socks[server_slot].peer_slot = client_slot;
  g_socks[server_slot].peer_eof = 0;

  if(pending_push_locked(listener_slot, server_slot) != 0){
    g_socks[client_slot].state = NET_STATE_CREATED;
    g_socks[client_slot].peer_slot = -1;
    g_socks[client_slot].peer_port = 0;
    free_slot_locked(server_slot);
    g_stats.connect_fail++;
    g_stats.drops++;
    net_unlock();
    errno = ECONNREFUSED;
    return -1;
  }

  g_stats.connect_ok++;
  net_unlock();

  tracef("connect", fd, remote_port);
  return 0;
}

int netkmod_accept(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
  int listener_slot;
  int accepted_slot;

  for(;;){
    net_lock();
    listener_slot = slot_from_fd_locked(fd);
    if(listener_slot < 0){
      net_unlock();
      errno = EBADF;
      return -1;
    }
    if(g_socks[listener_slot].state != NET_STATE_LISTEN){
      net_unlock();
      errno = EINVAL;
      return -1;
    }

    accepted_slot = pending_pop_locked(listener_slot);
    if(accepted_slot >= 0){
      if(addr && addrlen)
        (void)fill_sockaddr(g_socks[accepted_slot].peer_port, addr, addrlen);
      g_stats.accept_ok++;
      net_unlock();
      tracef("accept", fd, (uint32_t)slot_to_fd(accepted_slot));
      return slot_to_fd(accepted_slot);
    }

    if(g_socks[listener_slot].nonblock){
      net_unlock();
      errno = EWOULDBLOCK;
      return -1;
    }
    net_unlock();
    usleep(1000);
  }
}

ssize_t netkmod_send(int fd, const void *buf, size_t len, int flags)
{
  const uint8_t *src = (const uint8_t *)buf;
  size_t sent = 0;
  (void)flags;

  if(buf == 0 && len > 0){
    errno = EINVAL;
    return -1;
  }
  if(len == 0)
    return 0;

  for(;;){
    int slot;
    int peer;
    size_t wrote_now = 0;

    net_lock();
    slot = slot_from_fd_locked(fd);
    if(slot < 0){
      net_unlock();
      errno = EBADF;
      return -1;
    }
    if(g_socks[slot].state != NET_STATE_ESTABLISHED || g_socks[slot].peer_slot < 0){
      net_unlock();
      errno = ENOTCONN;
      return -1;
    }
    if(g_socks[slot].shut_wr){
      net_unlock();
      errno = EPIPE;
      return -1;
    }

    peer = g_socks[slot].peer_slot;
    if(peer < 0 || peer >= NETKMOD_MAX_SOCK || !g_socks[peer].used){
      g_socks[slot].peer_slot = -1;
      net_unlock();
      errno = EPIPE;
      return -1;
    }

    if(g_socks[peer].shut_rd){
      net_unlock();
      errno = EPIPE;
      return -1;
    }

    while(sent < len && g_socks[peer].rx_n < NETKMOD_RX_CAP){
      g_socks[peer].rx[g_socks[peer].rx_w] = src[sent];
      g_socks[peer].rx_w = (uint16_t)((g_socks[peer].rx_w + 1u) % NETKMOD_RX_CAP);
      g_socks[peer].rx_n++;
      sent++;
      wrote_now++;
    }

    if(wrote_now > 0){
      g_stats.tx_packets++;
      g_stats.tx_bytes += (uint32_t)wrote_now;
    }

    net_unlock();

    if(sent == len){
      tracef("send", fd, (uint32_t)sent);
      return (ssize_t)sent;
    }
    if(wrote_now > 0)
      continue;

    if(sent > 0)
      return (ssize_t)sent;

    net_lock();
    slot = slot_from_fd_locked(fd);
    if(slot < 0){
      net_unlock();
      errno = EBADF;
      return -1;
    }
    if(g_socks[slot].nonblock){
      net_unlock();
      errno = EWOULDBLOCK;
      return -1;
    }
    net_unlock();
    usleep(1000);
  }
}

ssize_t netkmod_recv(int fd, void *buf, size_t len, int flags)
{
  uint8_t *dst = (uint8_t *)buf;
  size_t got = 0;
  (void)flags;

  if(buf == 0 && len > 0){
    errno = EINVAL;
    return -1;
  }
  if(len == 0)
    return 0;

  for(;;){
    int slot;

    net_lock();
    slot = slot_from_fd_locked(fd);
    if(slot < 0){
      net_unlock();
      errno = EBADF;
      return -1;
    }

    while(got < len && g_socks[slot].rx_n > 0){
      dst[got] = g_socks[slot].rx[g_socks[slot].rx_r];
      g_socks[slot].rx_r = (uint16_t)((g_socks[slot].rx_r + 1u) % NETKMOD_RX_CAP);
      g_socks[slot].rx_n--;
      got++;
    }

    if(got > 0){
      g_stats.rx_packets++;
      g_stats.rx_bytes += (uint32_t)got;
      net_unlock();
      tracef("recv", fd, (uint32_t)got);
      return (ssize_t)got;
    }

    if(g_socks[slot].peer_eof || g_socks[slot].peer_slot < 0 || g_socks[slot].shut_rd){
      net_unlock();
      return 0;
    }

    if(g_socks[slot].nonblock){
      net_unlock();
      errno = EWOULDBLOCK;
      return -1;
    }

    net_unlock();
    usleep(1000);
  }
}

int netkmod_shutdown(int fd, int how)
{
  int slot;

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    errno = EBADF;
    return -1;
  }

  if(how == SHUT_RD || how == SHUT_RDWR)
    g_socks[slot].shut_rd = 1;

  if(how == SHUT_WR || how == SHUT_RDWR){
    int peer = g_socks[slot].peer_slot;
    g_socks[slot].shut_wr = 1;
    if(peer >= 0 && peer < NETKMOD_MAX_SOCK && g_socks[peer].used)
      g_socks[peer].peer_eof = 1;
  }

  if(how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR){
    net_unlock();
    errno = EINVAL;
    return -1;
  }

  net_unlock();
  tracef("shutdown", fd, (uint32_t)how);
  return 0;
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

  if(g_socks[slot].state == NET_STATE_LISTEN){
    while(g_socks[slot].pending_count > 0){
      int child = pending_pop_locked(slot);
      if(child >= 0 && child < NETKMOD_MAX_SOCK && g_socks[child].used){
        mark_peer_eof_locked(child);
        free_slot_locked(child);
      }
    }
  }

  mark_peer_eof_locked(slot);
  free_slot_locked(slot);
  net_unlock();

  tracef("close", fd, 0);
  return 0;
}

int netkmod_fcntl(int fd, int cmd, ...)
{
  va_list ap;
  int slot;
  int arg = 0;

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    if(fcntl_cmd_needs_arg(cmd)){
      va_start(ap, cmd);
      arg = va_arg(ap, int);
      va_end(ap);
      return __xv6_host_fcntl(fd, cmd, arg);
    }
    return __xv6_host_fcntl(fd, cmd);
  }

  switch(cmd){
#ifdef F_GETFL
  case F_GETFL:
    arg = O_RDWR | (g_socks[slot].nonblock ? O_NONBLOCK : 0);
    net_unlock();
    return arg;
#endif
#ifdef F_SETFL
  case F_SETFL:
    net_unlock();
    va_start(ap, cmd);
    arg = va_arg(ap, int);
    va_end(ap);
    net_lock();
    slot = slot_from_fd_locked(fd);
    if(slot < 0){
      net_unlock();
      errno = EBADF;
      return -1;
    }
    g_socks[slot].nonblock = ((arg & O_NONBLOCK) != 0);
    net_unlock();
    return 0;
#endif
#ifdef F_GETFD
  case F_GETFD:
    net_unlock();
    return 0;
#endif
#ifdef F_SETFD
  case F_SETFD:
    net_unlock();
    return 0;
#endif
  default:
    net_unlock();
    errno = EINVAL;
    return -1;
  }
}

int netkmod_ioctl(int fd, unsigned long request, ...)
{
  va_list ap;
  int slot;
  void *arg = 0;

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    if(ioctl_cmd_needs_arg(request)){
      va_start(ap, request);
      arg = va_arg(ap, void *);
      va_end(ap);
      return __xv6_host_ioctl(fd, request, arg);
    }
    return __xv6_host_ioctl(fd, request);
  }

#ifdef FIONBIO
  if(request == (unsigned long)FIONBIO){
    int on;
    net_unlock();
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);
    if(arg == 0){
      errno = EINVAL;
      return -1;
    }
    on = (*(int *)arg != 0);
    net_lock();
    slot = slot_from_fd_locked(fd);
    if(slot < 0){
      net_unlock();
      errno = EBADF;
      return -1;
    }
    g_socks[slot].nonblock = on;
    net_unlock();
    return 0;
  }
#endif

  net_unlock();
  errno = ENOTTY;
  return -1;
}

int netkmod_getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen)
{
  int slot;

  if(optval == 0 || optlen == 0){
    errno = EINVAL;
    return -1;
  }

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    errno = ENOTSOCK;
    return -1;
  }

  if(level == SOL_SOCKET && optname == SO_TYPE){
    int v = SOCK_STREAM;
    if(*optlen < (socklen_t)sizeof(v)){
      net_unlock();
      errno = EINVAL;
      return -1;
    }
    memcpy(optval, &v, sizeof(v));
    *optlen = (socklen_t)sizeof(v);
    net_unlock();
    return 0;
  }

  if(level == SOL_SOCKET && optname == SO_ERROR){
    int v = 0;
    if(*optlen < (socklen_t)sizeof(v)){
      net_unlock();
      errno = EINVAL;
      return -1;
    }
    memcpy(optval, &v, sizeof(v));
    *optlen = (socklen_t)sizeof(v);
    net_unlock();
    return 0;
  }

  net_unlock();
  errno = ENOPROTOOPT;
  return -1;
}

int netkmod_setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen)
{
  int slot;

  (void)optval;
  (void)optlen;

  net_lock();
  slot = slot_from_fd_locked(fd);
  net_unlock();
  if(slot < 0){
    errno = ENOTSOCK;
    return -1;
  }

  if(level == SOL_SOCKET && (optname == SO_REUSEADDR))
    return 0;

  errno = ENOPROTOOPT;
  return -1;
}

int netkmod_getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
  int slot;
  uint16_t port;

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    errno = ENOTSOCK;
    return -1;
  }
  port = g_socks[slot].local_port;
  net_unlock();

  return fill_sockaddr(port, addr, addrlen);
}

int netkmod_getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
  int slot;
  uint16_t port;

  net_lock();
  slot = slot_from_fd_locked(fd);
  if(slot < 0){
    net_unlock();
    errno = ENOTSOCK;
    return -1;
  }
  if(g_socks[slot].state != NET_STATE_ESTABLISHED){
    net_unlock();
    errno = ENOTCONN;
    return -1;
  }
  port = g_socks[slot].peer_port;
  net_unlock();

  return fill_sockaddr(port, addr, addrlen);
}

int netkmod_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
  int ready = 0;
  int elapsed = 0;

  if(fds == 0 && nfds > 0){
    errno = EINVAL;
    return -1;
  }

  while(1){
    nfds_t i;

    ready = 0;
    net_lock();
    for(i = 0; i < nfds; i++){
      int slot = slot_from_fd_locked(fds[i].fd);
      fds[i].revents = 0;
      if(slot < 0){
        fds[i].revents = POLLNVAL;
        ready++;
        continue;
      }
      if((fds[i].events & POLLIN) != 0){
        if(g_socks[slot].rx_n > 0 || g_socks[slot].peer_eof)
          fds[i].revents |= POLLIN;
      }
      if((fds[i].events & POLLOUT) != 0){
        if(!g_socks[slot].shut_wr && g_socks[slot].peer_slot >= 0)
          fds[i].revents |= POLLOUT;
      }
      if(g_socks[slot].peer_eof)
        fds[i].revents |= POLLHUP;
      if(fds[i].revents != 0)
        ready++;
    }
    net_unlock();

    if(ready > 0)
      return ready;
    if(timeout == 0)
      return 0;
    if(timeout > 0 && elapsed >= timeout)
      return 0;

    usleep(1000);
    elapsed++;
  }
}

int netkmod_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout)
{
  int fd;
  int ready = 0;
  int elapsed_ms = 0;
  int timeout_ms = -1;

  if(nfds < 0){
    errno = EINVAL;
    return -1;
  }

  if(timeout){
    if(timeout->tv_sec < 0 || timeout->tv_usec < 0){
      errno = EINVAL;
      return -1;
    }
    timeout_ms = (int)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
  }

  while(1){
    ready = 0;
    net_lock();
    for(fd = NETKMOD_FD_BASE; fd < NETKMOD_FD_BASE + NETKMOD_MAX_SOCK && fd < nfds; fd++){
      int slot = slot_from_fd_locked(fd);
      if(slot < 0)
        continue;

      if(readfds && FD_ISSET(fd, readfds)){
        if(g_socks[slot].rx_n > 0 || g_socks[slot].peer_eof)
          ready++;
        else
          FD_CLR(fd, readfds);
      }

      if(writefds && FD_ISSET(fd, writefds)){
        if(!g_socks[slot].shut_wr && g_socks[slot].peer_slot >= 0)
          ready++;
        else
          FD_CLR(fd, writefds);
      }

      if(exceptfds && FD_ISSET(fd, exceptfds))
        FD_CLR(fd, exceptfds);
    }
    net_unlock();

    if(ready > 0)
      return ready;
    if(timeout_ms == 0)
      return 0;
    if(timeout_ms > 0 && elapsed_ms >= timeout_ms)
      return 0;

    usleep(1000);
    elapsed_ms++;
  }
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
  tmp.active_sockets = count_active_sockets_locked();
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
  g_next_ephemeral = NETKMOD_EPHEMERAL_MIN;
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
  { "poll", (void *)netkmod_poll, XV6_MODULE_SYMBOL_OVERRIDE, 50 },
  { "select", (void *)netkmod_select, XV6_MODULE_SYMBOL_OVERRIDE, 50 },
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
