#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern int xv6_ptsname(int master_fd, char *out_path, int out_len);

int main(int argc, char **argv)
{
  char slave[32];
  int mfd;
  size_t n;

  if(argc != 2){
    printf("usage: ptysend text\n");
    return 1;
  }

  mfd = open("/dev/ptmx", O_RDWR);
  if(mfd < 0){
    printf("ptysend: open /dev/ptmx failed\n");
    return 1;
  }
  if(xv6_ptsname(mfd, slave, sizeof(slave)) != 0){
    close(mfd);
    printf("ptysend: ptsname failed\n");
    return 1;
  }

  n = strlen(argv[1]);
  if(write(mfd, argv[1], n) != (int)n || write(mfd, "\n", 1) != 1){
    close(mfd);
    printf("ptysend: write failed\n");
    return 1;
  }
  close(mfd);

  printf("%s\n", slave);
  return 0;
}
