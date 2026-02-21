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

static int has_prefix(const char *s, const char *p)
{
  while(*p){
    if(*s != *p)
      return 0;
    s++;
    p++;
  }
  return 1;
}

int main(int argc, char **argv)
{
  const char *in_path = 0;
  const char *out_path = 0;
  u32 bs = 512;
  u32 count = 1;
  char buf[1024];
  u32 total = 0;
  u32 i;
  int ifd;
  int ofd;

  for(i = 1; i < (u32)argc; i++){
    if(has_prefix(argv[i], "if="))
      in_path = argv[i] + 3;
    else if(has_prefix(argv[i], "of="))
      out_path = argv[i] + 3;
    else if(has_prefix(argv[i], "bs="))
      bs = parse_u32(argv[i] + 3);
    else if(has_prefix(argv[i], "count="))
      count = parse_u32(argv[i] + 6);
  }

  if(in_path == 0 || out_path == 0 || bs == 0 || bs > sizeof(buf)){
    printf("usage: dd if=/src of=/dst bs=N count=N\n");
    return 1;
  }

  ifd = xv6_open(in_path, O_RDONLY);
  if(ifd < 0){
    printf("dd: open input failed: %s\n", in_path);
    return 1;
  }
  ofd = xv6_open(out_path, O_WRONLY | O_CREAT | O_TRUNC);
  if(ofd < 0){
    xv6_close(ifd);
    printf("dd: open output failed: %s\n", out_path);
    return 1;
  }

  for(i = 0; i < count; i++){
    int n = xv6_read(ifd, buf, bs);
    if(n < 0){
      printf("dd: read failed\n");
      xv6_close(ifd);
      xv6_close(ofd);
      return 1;
    }
    if(n == 0)
      break;
    if(xv6_write(ofd, buf, (u32)n) != n){
      printf("dd: write failed\n");
      xv6_close(ifd);
      xv6_close(ofd);
      return 1;
    }
    total += (u32)n;
    if((u32)n < bs)
      break;
  }

  xv6_close(ifd);
  xv6_close(ofd);
  printf("%u bytes copied\n", total);
  return 0;
}
