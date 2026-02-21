#include "xv6_user.h"

static u32 parse_u32(const char *s)
{
  u32 v = 0;
  while(*s >= '0' && *s <= '9'){
    v = v * 10 + (u32)(*s - '0');
    s++;
  }
  return v;
}

int main(int argc, char **argv)
{
  char buf[128];
  const char *path;
  u32 need;

  if(argc != 4 || argv[1][0] != '-' || argv[1][1] != 'c'){
    printf("usage: head -c N /path\n");
    return 1;
  }

  need = parse_u32(argv[2]);
  path = argv[3];

  {
    int fd = xv6_open(path, O_RDONLY);
    if(fd < 0){
      printf("head: failed: %s\n", path);
      return 1;
    }
    while(need > 0){
      int n = xv6_read(fd, buf, (need < sizeof(buf)) ? need : (u32)sizeof(buf));
      if(n < 0){
        printf("head: read failed: %s\n", path);
        xv6_close(fd);
        return 1;
      }
      if(n == 0)
        break;
      if(xv6_write(1, buf, (u32)n) != n){
        xv6_close(fd);
        return 1;
      }
      need -= (u32)n;
    }
    xv6_close(fd);
  }

  return 0;
}
