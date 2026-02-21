typedef unsigned int u32;

extern int printf(const char *fmt, ...);
extern int xv6_open(const char *path, int flags);
extern int xv6_read(int fd, void *buf, u32 size);
extern int xv6_write(int fd, const void *buf, u32 size);
extern int xv6_close(int fd);
extern int xv6_ptsname(int master_fd, char *out_path, int out_len);

#define O_RDWR 0x0002

int main(void)
{
  char buf[64];
  char slave[32];
  int mfd = -1;
  int sfd = -1;
  int n;
  const char *a = "ping\n";
  const char *b = "pong\n";

  mfd = xv6_open("/dev/ptmx", O_RDWR);
  if(mfd < 0){
    printf("ptydemo: open /dev/ptmx failed\n");
    return 1;
  }
  if(xv6_ptsname(mfd, slave, sizeof(slave)) != 0){
    xv6_close(mfd);
    printf("ptydemo: ptsname failed\n");
    return 1;
  }
  sfd = xv6_open(slave, O_RDWR);
  if(sfd < 0){
    xv6_close(mfd);
    printf("ptydemo: open %s failed\n", slave);
    return 1;
  }

  if(xv6_write(mfd, a, 5) != 5){
    printf("ptydemo: write master failed\n");
    xv6_close(sfd);
    xv6_close(mfd);
    return 1;
  }
  n = xv6_read(sfd, buf, sizeof(buf) - 1);
  if(n <= 0){
    printf("ptydemo: read slave failed\n");
    xv6_close(sfd);
    xv6_close(mfd);
    return 1;
  }
  buf[n] = 0;
  printf("slave:%s", buf);

  if(xv6_write(sfd, b, 5) != 5){
    printf("ptydemo: write slave failed\n");
    xv6_close(sfd);
    xv6_close(mfd);
    return 1;
  }
  n = xv6_read(mfd, buf, sizeof(buf) - 1);
  if(n <= 0){
    printf("ptydemo: read master failed\n");
    xv6_close(sfd);
    xv6_close(mfd);
    return 1;
  }
  buf[n] = 0;
  printf("master:%s", buf);

  xv6_close(sfd);
  xv6_close(mfd);
  return 0;
}
