#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xv6_socket_compat.h"

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

extern int netkmod_get_stats(netkmod_stats_export_t *out, uint32_t out_size) __attribute__((weak));
extern int netkmod_set_trace(int enabled) __attribute__((weak));

static unsigned short host_to_be16(unsigned short v)
{
  return (unsigned short)((v >> 8) | (v << 8));
}

static unsigned int host_to_be32(unsigned int v)
{
  return ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) | ((v & 0x00ff0000u) >> 8) |
         ((v & 0xff000000u) >> 24);
}

static void close_fd(int *fd)
{
  if(fd && *fd >= 0){
    (void)close(*fd);
    *fd = -1;
  }
}

static int mk_loopback_addr(struct sockaddr_in *sin, unsigned short port)
{
  if(sin == 0)
    return -1;
  memset(sin, 0, sizeof(*sin));
  sin->sin_len = (uint8_t)sizeof(*sin);
  sin->sin_family = AF_INET;
  sin->sin_port = host_to_be16(port);
  sin->sin_addr.s_addr = host_to_be32(INADDR_LOOPBACK);
  return 0;
}

static int mk_loopback6_addr(struct sockaddr_in6 *sin6, unsigned short port)
{
  if(sin6 == 0)
    return -1;
  memset(sin6, 0, sizeof(*sin6));
  sin6->sin6_len = (uint8_t)sizeof(*sin6);
  sin6->sin6_family = AF_INET6;
  sin6->sin6_port = host_to_be16(port);
  sin6->sin6_addr.s6_addr[15] = 1;
  return 0;
}

static int parse_port(const char *s, unsigned short *out)
{
  long v;
  char *end = 0;

  if(s == 0 || out == 0)
    return -1;

  errno = 0;
  v = strtol(s, &end, 10);
  if(errno != 0 || end == s || (end && *end != 0) || v <= 0 || v > 65535)
    return -1;

  *out = (unsigned short)v;
  return 0;
}

static void print_stats(void)
{
  netkmod_stats_export_t st;

  if(netkmod_get_stats == 0){
    puts("net_diag: netkmod_get_stats symbol missing (module not loaded?)");
    return;
  }

  memset(&st, 0, sizeof(st));
  if(netkmod_get_stats(&st, (uint32_t)sizeof(st)) != 0){
    printf("net_diag: stats failed errno=%d\n", errno);
    return;
  }

  printf("net_diag: stats created=%u closed=%u active=%u connect_ok=%u connect_fail=%u accept_ok=%u\n", st.sockets_created,
         st.sockets_closed, st.active_sockets, st.connect_ok, st.connect_fail, st.accept_ok);
  printf("net_diag: io tx_packets=%u rx_packets=%u tx_bytes=%u rx_bytes=%u drops=%u\n", st.tx_packets, st.rx_packets,
         st.tx_bytes, st.rx_bytes, st.drops);
}

static int stream_selftest(unsigned short port, const char *label, int use_poll, int use_select)
{
  struct sockaddr_in addr;
  const char *msg = "diag-ping";
  const char *step = "init";
  char inbuf[32];
  int listener = -1;
  int client = -1;
  int server = -1;
  int n;

  if(mk_loopback_addr(&addr, port) != 0)
    return 1;

  listener = socket(AF_INET, SOCK_STREAM, 0);
  if(listener < 0)
    goto fail;
  step = "bind";
  if(bind(listener, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0)
    goto fail;
  step = "listen";
  if(listen(listener, 2) != 0)
    goto fail;

  step = "socket-client";
  client = socket(AF_INET, SOCK_STREAM, 0);
  if(client < 0)
    goto fail;
  step = "connect";
  if(connect(client, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0)
    goto fail;

  step = "accept";
  server = accept(listener, 0, 0);
  if(server < 0)
    goto fail;

  if(use_poll){
    struct pollfd pfd;
    int rc;
    pfd.fd = client;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    step = "poll-write";
    rc = poll(&pfd, 1, 3000);
    if(rc <= 0 || (pfd.revents & POLLOUT) == 0)
      goto fail;
  }

  step = "send";
  n = send(client, msg, strlen(msg), 0);
  if(n != (int)strlen(msg))
    goto fail;

  if(use_poll){
    struct pollfd pfd;
    int rc;
    pfd.fd = server;
    pfd.events = POLLIN;
    pfd.revents = 0;
    step = "poll-read";
    rc = poll(&pfd, 1, 3000);
    if(rc <= 0 || (pfd.revents & POLLIN) == 0)
      goto fail;
  }

  if(use_select){
    fd_set rfds;
    fd_set wfds;
    struct timeval tv;
    int maxfd = (server > client) ? server : client;
    int rc;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_SET(server, &rfds);
    FD_SET(client, &wfds);
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    step = "select";
    rc = select(maxfd + 1, &rfds, &wfds, 0, &tv);
    if(rc <= 0 || !FD_ISSET(server, &rfds) || !FD_ISSET(client, &wfds))
      goto fail;
  }

  memset(inbuf, 0, sizeof(inbuf));
  step = "recv";
  n = recv(server, inbuf, sizeof(inbuf) - 1, 0);
  if(n <= 0)
    goto fail;
  inbuf[n] = 0;
  if(strcmp(inbuf, msg) != 0)
    goto fail;

  close_fd(&server);
  close_fd(&client);
  close_fd(&listener);
  printf("net_diag: %s selftest ok port=%u\n", label, (unsigned)port);
  return 0;

fail:
  printf("net_diag: %s selftest failed step=%s errno=%d\n", label, step, errno);
  close_fd(&server);
  close_fd(&client);
  close_fd(&listener);
  return 1;
}

static int udp_selftest(unsigned short port, int ipv6)
{
  const char *msg = "diag-udp-ping";
  const char *step = "init";
  char inbuf[48];
  char outbuf[56];
  union {
    struct sockaddr_in v4;
    struct sockaddr_in6 v6;
  } addr;
  union {
    struct sockaddr_in v4;
    struct sockaddr_in6 v6;
  } peer;
  struct sockaddr *sa;
  socklen_t sa_len;
  socklen_t peer_len;
  int server = -1;
  int client = -1;
  int domain = ipv6 ? AF_INET6 : AF_INET;
  int n;

  if(ipv6){
    if(mk_loopback6_addr(&addr.v6, port) != 0)
      return 1;
    sa = (struct sockaddr *)&addr.v6;
    sa_len = (socklen_t)sizeof(addr.v6);
  } else {
    if(mk_loopback_addr(&addr.v4, port) != 0)
      return 1;
    sa = (struct sockaddr *)&addr.v4;
    sa_len = (socklen_t)sizeof(addr.v4);
  }

  server = socket(domain, SOCK_DGRAM, 0);
  if(server < 0)
    goto fail;
  step = "bind";
  if(bind(server, sa, sa_len) != 0)
    goto fail;

  step = "socket-client";
  client = socket(domain, SOCK_DGRAM, 0);
  if(client < 0)
    goto fail;
  step = "connect";
  if(connect(client, sa, sa_len) != 0)
    goto fail;

  step = "send";
  n = send(client, msg, strlen(msg), 0);
  if(n != (int)strlen(msg))
    goto fail;

  memset(&peer, 0, sizeof(peer));
  peer_len = (socklen_t)sizeof(peer);
  memset(inbuf, 0, sizeof(inbuf));
  step = "recvfrom";
  n = recvfrom(server, inbuf, sizeof(inbuf) - 1, 0, (struct sockaddr *)&peer, &peer_len);
  if(n <= 0)
    goto fail;
  inbuf[n] = 0;
  if(strcmp(inbuf, msg) != 0)
    goto fail;

  snprintf(outbuf, sizeof(outbuf), "ack:%s", inbuf);
  step = "sendto";
  n = sendto(server, outbuf, strlen(outbuf), 0, (const struct sockaddr *)&peer, peer_len);
  if(n != (int)strlen(outbuf))
    goto fail;

  memset(inbuf, 0, sizeof(inbuf));
  step = "recv";
  n = recv(client, inbuf, sizeof(inbuf) - 1, 0);
  if(n <= 0)
    goto fail;
  inbuf[n] = 0;
  if(strcmp(inbuf, outbuf) != 0)
    goto fail;

  close_fd(&client);
  close_fd(&server);
  printf("net_diag: udp%s selftest ok port=%u\n", ipv6 ? "6" : "", (unsigned)port);
  return 0;

fail:
  printf("net_diag: udp%s selftest failed step=%s errno=%d\n", ipv6 ? "6" : "", step, errno);
  close_fd(&client);
  close_fd(&server);
  return 1;
}

static int set_trace(const char *v)
{
  int en;

  if(v == 0)
    return 1;

  if(strcmp(v, "1") == 0 || strcmp(v, "on") == 0)
    en = 1;
  else if(strcmp(v, "0") == 0 || strcmp(v, "off") == 0)
    en = 0;
  else
    return 1;

  if(netkmod_set_trace == 0){
    puts("net_diag: netkmod_set_trace symbol missing (module not loaded?)");
    return 1;
  }

  if(netkmod_set_trace(en) != 0){
    printf("net_diag: set_trace failed errno=%d\n", errno);
    return 1;
  }

  printf("net_diag: trace=%d\n", en);
  return 0;
}

static void usage(void)
{
  puts("usage: net_diag [stats|selftest [port]|udp [port]|udp6 [port]|poll [port]|select [port]|trace <0|1|on|off>]");
}

int main(int argc, char **argv)
{
  unsigned short port = 24631;

  if(argc == 1){
    print_stats();
    return stream_selftest(port, "tcp", 0, 0);
  }

  if(strcmp(argv[1], "stats") == 0){
    print_stats();
    return 0;
  }

  if(argc >= 3 && strcmp(argv[1], "trace") != 0 && parse_port(argv[2], &port) != 0){
    puts("net_diag: bad port");
    return 1;
  }

  if(strcmp(argv[1], "selftest") == 0)
    return stream_selftest(port, "tcp", 0, 0);
  if(strcmp(argv[1], "udp") == 0)
    return udp_selftest(port, 0);
  if(strcmp(argv[1], "udp6") == 0)
    return udp_selftest(port, 1);
  if(strcmp(argv[1], "poll") == 0)
    return stream_selftest(port, "poll", 1, 0);
  if(strcmp(argv[1], "select") == 0)
    return stream_selftest(port, "select", 0, 1);
  if(strcmp(argv[1], "trace") == 0){
    if(argc < 3){
      usage();
      return 1;
    }
    return set_trace(argv[2]);
  }

  usage();
  return 1;
}
