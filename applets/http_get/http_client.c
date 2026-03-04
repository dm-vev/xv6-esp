#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>

#define RECV_BUF_SIZE 8192

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
  struct hostent *he;
  struct sockaddr_in sa;
  
  if(!host || !path)
    return -1;
    
  he = gethostbyname(host);
  if(he == NULL)
    return -1;
    
  sock = socket(AF_INET, SOCK_STREAM, 0);
  if(sock < 0)
    return -1;
    
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  memcpy(&sa.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
  
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
