#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static int parse_u32(const char *s, uint32_t *out)
{
  uint32_t v = 0;
  if(s == 0 || *s == 0 || out == 0)
    return -1;
  while(*s){
    if(*s < '0' || *s > '9')
      return -1;
    v = v * 10u + (uint32_t)(*s - '0');
    s++;
  }
  *out = v;
  return 0;
}

int main(int argc, char **argv)
{
  char buf[128];
  uint32_t need = 0;
  if(argc != 2 || parse_u32(argv[1], &need) != 0){
    printf("usage: stdinhead N\n");
    return 1;
  }
  while(need > 0){
    uint32_t want = (need < sizeof(buf)) ? need : (uint32_t)sizeof(buf);
    int n = read(0, buf, want);
    if(n < 0)
      return 1;
    if(n == 0)
      break;
    if(write(1, buf, (uint32_t)n) != n)
      return 1;
    need -= (uint32_t)n;
  }
  return 0;
}
