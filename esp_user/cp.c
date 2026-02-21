#include "xv6_user.h"

int main(int argc, char **argv)
{
  char buf[512];
  int ifd;
  int ofd;

  if(argc != 3){
    printf("usage: cp /src /dst\n");
    return 1;
  }

  ifd = xv6_open(argv[1], O_RDONLY);
  if(ifd < 0){
    printf("cp: open failed: %s\n", argv[1]);
    return 1;
  }

  ofd = xv6_open(argv[2], O_WRONLY | O_CREAT | O_TRUNC);
  if(ofd < 0){
    xv6_close(ifd);
    printf("cp: open failed: %s\n", argv[2]);
    return 1;
  }

  for(;;){
    int n = xv6_read(ifd, buf, sizeof(buf));
    if(n < 0){
      printf("cp: read failed: %s\n", argv[1]);
      xv6_close(ifd);
      xv6_close(ofd);
      return 1;
    }
    if(n == 0)
      break;
    if(xv6_write(ofd, buf, (u32)n) != n){
      printf("cp: write failed: %s\n", argv[2]);
      xv6_close(ifd);
      xv6_close(ofd);
      return 1;
    }
  }

  xv6_close(ifd);
  xv6_close(ofd);
  return 0;
}
