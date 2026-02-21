#include "xv6_user.h"

int main(int argc, char **argv)
{
  char slave[32];
  int mfd;
  u32 n;

  if(argc != 2){
    printf("usage: ptysend text\n");
    return 1;
  }

  mfd = xv6_open("/dev/ptmx", O_RDWR);
  if(mfd < 0){
    printf("ptysend: open /dev/ptmx failed\n");
    return 1;
  }
  if(xv6_ptsname(mfd, slave, sizeof(slave)) != 0){
    xv6_close(mfd);
    printf("ptysend: ptsname failed\n");
    return 1;
  }

  n = strlen(argv[1]);
  if(xv6_write(mfd, argv[1], n) != (int)n || xv6_write(mfd, "\n", 1) != 1){
    xv6_close(mfd);
    printf("ptysend: write failed\n");
    return 1;
  }
  xv6_close(mfd);

  printf("%s\n", slave);
  return 0;
}
