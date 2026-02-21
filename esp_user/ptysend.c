typedef unsigned int u32;

extern int printf(const char *fmt, ...);
extern int xv6_open(const char *path, int flags);
extern int xv6_write(int fd, const void *buf, u32 size);
extern int xv6_close(int fd);
extern int xv6_ptsname(int master_fd, char *out_path, int out_len);
extern unsigned int strlen(const char *s);

#define O_RDWR 0x0002

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
