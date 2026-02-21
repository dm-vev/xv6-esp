typedef unsigned int u32;

extern int printf(const char *fmt, ...);
extern int xv6_open(const char *path, int flags);
extern int xv6_read(int fd, void *buf, u32 size);
extern int xv6_close(int fd);
extern int xv6_write(int fd, const void *buf, u32 size);

#define O_RDWR 0x0002

int main(int argc, char **argv)
{
  char buf[128];
  int sfd;
  int n;

  if(argc != 2){
    printf("usage: ptyrecv /dev/pts/N\n");
    return 1;
  }

  sfd = xv6_open(argv[1], O_RDWR);
  if(sfd < 0){
    printf("ptyrecv: open failed: %s\n", argv[1]);
    return 1;
  }

  n = xv6_read(sfd, buf, sizeof(buf));
  if(n < 0){
    xv6_close(sfd);
    printf("ptyrecv: read failed\n");
    return 1;
  }

  if(n > 0)
    (void)xv6_write(1, buf, (u32)n);
  xv6_close(sfd);
  return 0;
}
