extern int printf(const char *fmt, ...);
extern unsigned int strlen(const char *s);
extern void *memcpy(void *dst, const void *src, unsigned int n);
extern int xv6_open(const char *path, int flags);
extern int xv6_write(int fd, const void *buf, unsigned int size);
extern int xv6_close(int fd);

#define O_WRONLY 0x0001
#define O_CREAT  0x0200
#define O_TRUNC  0x0400

int main(int argc, char **argv)
{
  char buf[256];
  unsigned int n = 0;
  int fd;
  int i;

  if(argc < 3){
    printf("usage: write /path text...\n");
    return 1;
  }

  for(i = 2; i < argc; i++){
    int len = strlen(argv[i]);
    if(n + (unsigned)len + 2 >= sizeof(buf))
      break;
    memcpy(buf + n, argv[i], (unsigned)len);
    n += (unsigned)len;
    if(i + 1 < argc)
      buf[n++] = ' ';
  }
  buf[n++] = '\n';

  fd = xv6_open(argv[1], O_WRONLY | O_CREAT | O_TRUNC);
  if(fd < 0){
    printf("write: failed: %s\n", argv[1]);
    return 1;
  }
  if(xv6_write(fd, buf, n) != (int)n){
    xv6_close(fd);
    printf("write: failed: %s\n", argv[1]);
    return 1;
  }
  xv6_close(fd);
  return 0;
}
