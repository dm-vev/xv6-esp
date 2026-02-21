typedef unsigned int u32;

extern int printf(const char *fmt, ...);
extern int xv6_open(const char *path, int flags);
extern int xv6_read(int fd, void *buf, u32 size);
extern int xv6_write(int fd, const void *buf, u32 size);
extern int xv6_close(int fd);

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_CREAT  0x0200
#define O_TRUNC  0x0400

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
