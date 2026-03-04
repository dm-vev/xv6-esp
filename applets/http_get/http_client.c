#include "http_client.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "xv6_socket_compat.h"

#define RECV_BUF_SIZE 8192

static unsigned short host_to_be16(unsigned short v)
{
  return (unsigned short)((v >> 8) | (v << 8));
}

static unsigned int host_to_be32(unsigned int v)
{
  return ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) | ((v & 0x00ff0000u) >> 8) |
         ((v & 0xff000000u) >> 24);
}

static int parse_ipv4_host(const char *host, unsigned int *out_be_addr)
{
  unsigned int a, b, c, d;
  char tail = 0;

  if(host == NULL || out_be_addr == NULL)
    return -1;
  if(sscanf(host, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4)
    return -1;
  if(a > 255u || b > 255u || c > 255u || d > 255u)
    return -1;

  *out_be_addr = host_to_be32((a << 24) | (b << 16) | (c << 8) | d);
  return 0;
}

static int http_send_request(int sock, const char *host, const char *path)
{
  char req[512];
  int len = snprintf(req, sizeof(req),
    "GET %s HTTP/1.0\r\n"
    "Host: %s\r\n"
    "User-Agent: xv6-httpget/1.0\r\n"
    "Connection: close\r\n"
    "\r\n",
    path, host);
  
  return write(sock, req, (size_t)len);
}

static int http_read_response(int sock, FILE *out)
{
  char buf[RECV_BUF_SIZE];
  char *body_start = NULL;
  int received;
  int header_done = 0;
  
  while((received = read(sock, buf, sizeof(buf) - 1)) > 0){
    buf[received] = 0;
    
    if(!header_done){
      body_start = strstr(buf, "\r\n\r\n");
      if(body_start != NULL){
        char *header_end = strstr(buf, "\r\n");
        if(header_end){
          *header_end = 0;
          printf("%s\n", buf);
        }
        
        body_start += 4;
        int body_len = received - (int)(body_start - buf);
        if(out && body_len > 0)
          fwrite(body_start, 1, (size_t)body_len, out);
        
        header_done = 1;
      } else {
        char *line_end = strstr(buf, "\r\n");
        if(line_end){
          *line_end = 0;
          printf("%s\n", buf);
        }
      }
    } else {
      if(out)
        fwrite(buf, 1, (size_t)received, out);
    }
  }
  
  if(out)
    fflush(out);
    
  return received < 0 ? -1 : 0;
}

int http_client_get(const char *host, int port, const char *path, FILE *out)
{
  int sock;
  struct sockaddr_in sa;
  unsigned int be_addr;
  
  if(!host || !path)
    return -1;

  if(parse_ipv4_host(host, &be_addr) != 0){
    errno = EINVAL;
    return -1;
  }
    
  sock = socket(AF_INET, SOCK_STREAM, 0);
  if(sock < 0)
    return -1;
    
  memset(&sa, 0, sizeof(sa));
  sa.sin_len = (uint8_t)sizeof(sa);
  sa.sin_family = AF_INET;
  sa.sin_port = host_to_be16((unsigned short)port);
  sa.sin_addr.s_addr = be_addr;
  
  if(connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0){
    close(sock);
    return -1;
  }
  
  if(http_send_request(sock, host, path) < 0){
    close(sock);
    return -1;
  }
  
  int rc = http_read_response(sock, out);
  close(sock);
  
  return rc;
}
