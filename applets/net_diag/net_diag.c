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

static int mk_loopback_addr(struct sockaddr_in *sin, unsigned short port)
{
  if(sin == 0)
    return -1;
  memset(sin, 0, sizeof(*sin));
  sin->sin_family = AF_INET;
  sin->sin_port = host_to_be16(port);
  sin->sin_addr.s_addr = host_to_be32(INADDR_LOOPBACK);
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

  printf("net_diag: stats created=%u closed=%u active=%u connect_ok=%u connect_fail=%u accept_ok=%u\n",
         st.sockets_created, st.sockets_closed, st.active_sockets, st.connect_ok, st.connect_fail, st.accept_ok);
  printf("net_diag: io tx_packets=%u rx_packets=%u tx_bytes=%u rx_bytes=%u drops=%u\n", st.tx_packets,
         st.rx_packets, st.tx_bytes, st.rx_bytes, st.drops);
}

static int run_selftest(unsigned short port)
{
  struct sockaddr_in addr;
  const char *msg = "diag-ping";
  char inbuf[64];
  int listener = -1;
  int client = -1;
  int server = -1;
  int n;

  if(mk_loopback_addr(&addr, port) != 0){
    puts("net_diag: selftest bad addr");
    return 1;
  }

  listener = socket(AF_INET, SOCK_STREAM, 0);
  if(listener < 0){
    printf("net_diag: socket(listener) failed errno=%d\n", errno);
    return 1;
  }

  if(bind(listener, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0){
    printf("net_diag: bind failed errno=%d\n", errno);
    goto fail;
  }

  if(listen(listener, 4) != 0){
    printf("net_diag: listen failed errno=%d\n", errno);
    goto fail;
  }

  client = socket(AF_INET, SOCK_STREAM, 0);
  if(client < 0){
    printf("net_diag: socket(client) failed errno=%d\n", errno);
    goto fail;
  }

  if(connect(client, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0){
    printf("net_diag: connect failed errno=%d\n", errno);
    goto fail;
  }

  server = accept(listener, 0, 0);
  if(server < 0){
    printf("net_diag: accept failed errno=%d\n", errno);
    goto fail;
  }

  n = send(client, msg, strlen(msg), 0);
  if(n != (int)strlen(msg)){
    printf("net_diag: send(client) failed n=%d errno=%d\n", n, errno);
    goto fail;
  }

  memset(inbuf, 0, sizeof(inbuf));
  n = recv(server, inbuf, sizeof(inbuf) - 1, 0);
  if(n <= 0){
    printf("net_diag: recv(server) failed n=%d errno=%d\n", n, errno);
    goto fail;
  }
  inbuf[n] = 0;

  if(strcmp(inbuf, msg) != 0){
    printf("net_diag: payload mismatch got='%s' expected='%s'\n", inbuf, msg);
    goto fail;
  }

  if(close(server) != 0)
    printf("net_diag: close(server) errno=%d\n", errno);
  if(close(client) != 0)
    printf("net_diag: close(client) errno=%d\n", errno);
  if(close(listener) != 0)
    printf("net_diag: close(listener) errno=%d\n", errno);

  printf("net_diag: selftest ok port=%u\n", (unsigned)port);
  return 0;

fail:
  if(server >= 0)
    (void)close(server);
  if(client >= 0)
    (void)close(client);
  if(listener >= 0)
    (void)close(listener);
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
  puts("usage: net_diag [stats|selftest [port]|trace <0|1|on|off>]");
}

int main(int argc, char **argv)
{
  unsigned short port = 24631;

  if(argc == 1){
    print_stats();
    return run_selftest(port);
  }

  if(strcmp(argv[1], "stats") == 0){
    print_stats();
    return 0;
  }

  if(strcmp(argv[1], "selftest") == 0){
    if(argc >= 3 && parse_port(argv[2], &port) != 0){
      puts("net_diag: bad port");
      return 1;
    }
    return run_selftest(port);
  }

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
