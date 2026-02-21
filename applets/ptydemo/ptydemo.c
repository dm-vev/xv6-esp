#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

extern int xv6_ptsname(int master_fd, char *out_path, int out_len);

int main(void)
{
  char buf[64];
  char slave[32];
  int mfd = -1;
  int sfd = -1;
  int n;
  const char *a = "ping\n";
  const char *b = "pong\n";

  mfd = open("/dev/ptmx", O_RDWR);
  if(mfd < 0){
    printf("ptydemo: open /dev/ptmx failed\n");
    return 1;
  }
  if(xv6_ptsname(mfd, slave, sizeof(slave)) != 0){
    close(mfd);
    printf("ptydemo: ptsname failed\n");
    return 1;
  }
  sfd = open(slave, O_RDWR);
  if(sfd < 0){
    close(mfd);
    printf("ptydemo: open %s failed\n", slave);
    return 1;
  }

  if(write(mfd, a, 5) != 5){
    printf("ptydemo: write master failed\n");
    close(sfd);
    close(mfd);
    return 1;
  }
  n = read(sfd, buf, sizeof(buf) - 1);
  if(n <= 0){
    printf("ptydemo: read slave failed\n");
    close(sfd);
    close(mfd);
    return 1;
  }
  buf[n] = 0;
  printf("slave:%s", buf);

  if(write(sfd, b, 5) != 5){
    printf("ptydemo: write slave failed\n");
    close(sfd);
    close(mfd);
    return 1;
  }
  n = read(mfd, buf, sizeof(buf) - 1);
  if(n <= 0){
    printf("ptydemo: read master failed\n");
    close(sfd);
    close(mfd);
    return 1;
  }
  buf[n] = 0;
  printf("master:%s", buf);

  close(sfd);
  close(mfd);
  return 0;
}
