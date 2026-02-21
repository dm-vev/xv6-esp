#include "xv6_user.h"

static int parse_u32(const char *s, u32 *out)
{
  u32 v = 0;
  if(s == 0 || *s == 0 || out == 0)
    return -1;
  while(*s){
    if(*s < '0' || *s > '9')
      return -1;
    v = v * 10u + (u32)(*s - '0');
    s++;
  }
  *out = v;
  return 0;
}

int main(int argc, char **argv)
{
  char buf[128];
  u32 need = 0;
  if(argc != 2 || parse_u32(argv[1], &need) != 0){
    printf("usage: stdinhead N\n");
    return 1;
  }
  while(need > 0){
    u32 want = (need < sizeof(buf)) ? need : (u32)sizeof(buf);
    int n = xv6_read(0, buf, want);
    if(n < 0)
      return 1;
    if(n == 0)
      break;
    if(xv6_write(1, buf, (u32)n) != n)
      return 1;
    need -= (u32)n;
  }
  return 0;
}
