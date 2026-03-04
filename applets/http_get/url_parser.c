#include "url_parser.h"
#include <string.h>
#include <stdlib.h>

int url_parse(const char *url, char *host, size_t host_len, 
              char *path, size_t path_len, int *port)
{
  const char *p = url;
  const char *host_start;
  const char *path_start;
  const char *port_start;
  size_t host_len_calc;
  int default_port = 80;

  if(!url || !host || !path || !port)
    return -1;

  if(strncmp(p, "http://", 7) == 0){
    p += 7;
    default_port = 80;
  } else if(strncmp(p, "https://", 8) == 0){
    return -1;
  }

  host_start = p;
  path_start = strchr(p, '/');
  
  if(path_start == NULL){
    path_start = p + strlen(p);
  }
  
  host_len_calc = (size_t)(path_start - host_start);
  
  port_start = strchr(host_start, ':');
  if(port_start != NULL && port_start < path_start){
    host_len_calc = (size_t)(port_start - host_start);
    default_port = atoi(port_start + 1);
  }
  
  if(host_len_calc >= host_len)
    return -1;
  
  memcpy(host, host_start, host_len_calc);
  host[host_len_calc] = 0;
  
  if(*path_start == '/'){
    size_t path_len_calc = strlen(path_start);
    if(path_len_calc >= path_len)
      path_len_calc = path_len - 1;
    memcpy(path, path_start, path_len_calc);
    path[path_len_calc] = 0;
  } else {
    path[0] = '/';
    path[1] = 0;
  }
  
  *port = default_port;
  
  return 0;
}
