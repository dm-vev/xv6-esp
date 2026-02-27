#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
  unsigned int sockets_created;
  unsigned int sockets_closed;
  unsigned int connect_ok;
  unsigned int connect_fail;
  unsigned int accept_ok;
  unsigned int tx_packets;
  unsigned int rx_packets;
  unsigned int tx_bytes;
  unsigned int rx_bytes;
  unsigned int drops;
  unsigned int active_sockets;
} netkmod_stats_export_t;

extern int netkmod_get_stats(netkmod_stats_export_t *out, unsigned int out_size);

static int mk_loopback_addr(struct sockaddr_in *sin, unsigned short port)
{
  if(sin == 0)
    return -1;
  memset(sin, 0, sizeof(*sin));
  sin->sin_family = AF_INET;
  sin->sin_port = htons(port);
  sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return 0;
}

int main(void)
{
  const char *msg = "ping-from-client";
  char inbuf[64];
  char outbuf[64];
  struct sockaddr_in addr;
  int listener = -1;
  int client = -1;
  int server = -1;
  int n;
  netkmod_stats_export_t st;

  if(mk_loopback_addr(&addr, 24567) != 0){
    puts("tcp_loop_test: bad addr");
    return 1;
  }

  listener = socket(AF_INET, SOCK_STREAM, 0);
  if(listener < 0){
    printf("tcp_loop_test: socket(listener) failed errno=%d\n", errno);
    return 1;
  }

  if(bind(listener, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0){
    printf("tcp_loop_test: bind failed errno=%d\n", errno);
    return 1;
  }

  if(listen(listener, 4) != 0){
    printf("tcp_loop_test: listen failed errno=%d\n", errno);
    return 1;
  }

  client = socket(AF_INET, SOCK_STREAM, 0);
  if(client < 0){
    printf("tcp_loop_test: socket(client) failed errno=%d\n", errno);
    return 1;
  }

  if(connect(client, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0){
    printf("tcp_loop_test: connect failed errno=%d\n", errno);
    return 1;
  }

  server = accept(listener, 0, 0);
  if(server < 0){
    printf("tcp_loop_test: accept failed errno=%d\n", errno);
    return 1;
  }

  n = (int)send(client, msg, strlen(msg), 0);
  if(n != (int)strlen(msg)){
    printf("tcp_loop_test: send failed n=%d errno=%d\n", n, errno);
    return 1;
  }

  memset(inbuf, 0, sizeof(inbuf));
  n = (int)recv(server, inbuf, sizeof(inbuf) - 1, 0);
  if(n <= 0){
    printf("tcp_loop_test: recv(server) failed n=%d errno=%d\n", n, errno);
    return 1;
  }

  inbuf[n] = 0;
  if(strcmp(inbuf, msg) != 0){
    printf("tcp_loop_test: payload mismatch got='%s' expected='%s'\n", inbuf, msg);
    return 1;
  }

  snprintf(outbuf, sizeof(outbuf), "echo:%s", inbuf);
  n = (int)send(server, outbuf, strlen(outbuf), 0);
  if(n != (int)strlen(outbuf)){
    printf("tcp_loop_test: send(server) failed n=%d errno=%d\n", n, errno);
    return 1;
  }

  memset(inbuf, 0, sizeof(inbuf));
  n = (int)recv(client, inbuf, sizeof(inbuf) - 1, 0);
  if(n <= 0){
    printf("tcp_loop_test: recv(client) failed n=%d errno=%d\n", n, errno);
    return 1;
  }
  inbuf[n] = 0;

  if(strcmp(inbuf, outbuf) != 0){
    printf("tcp_loop_test: echo mismatch got='%s' expected='%s'\n", inbuf, outbuf);
    return 1;
  }

  if(close(server) != 0)
    printf("tcp_loop_test: close(server) errno=%d\n", errno);
  if(close(client) != 0)
    printf("tcp_loop_test: close(client) errno=%d\n", errno);
  if(close(listener) != 0)
    printf("tcp_loop_test: close(listener) errno=%d\n", errno);

  memset(&st, 0, sizeof(st));
  if(netkmod_get_stats(&st, (unsigned int)sizeof(st)) == 0){
    printf("tcp_loop_test: stats created=%u closed=%u active=%u tx=%u rx=%u\n", st.sockets_created,
           st.sockets_closed, st.active_sockets, st.tx_bytes, st.rx_bytes);
  }

  puts("tcp_loop_test: ok");
  return 0;
}
