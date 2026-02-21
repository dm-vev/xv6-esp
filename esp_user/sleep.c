typedef unsigned int u32;

extern int printf(const char *fmt, ...);
extern int usleep(unsigned int usec);

static int parse_u32(const char *s, u32 *out)
{
  u32 v = 0;
  if(s == 0 || *s == 0 || out == 0)
    return -1;
  while(*s){
    if(*s < '0' || *s > '9')
      return -1;
    v = v * 10 + (u32)(*s - '0');
    s++;
  }
  *out = v;
  return 0;
}

int main(int argc, char **argv)
{
  u32 ms = 0;
  if(argc != 2 || parse_u32(argv[1], &ms) != 0){
    printf("usage: sleep ms\n");
    return 1;
  }
  (void)usleep(ms * 1000u);
  return 0;
}
