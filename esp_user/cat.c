typedef unsigned int u32;

extern int printf(const char *fmt, ...);
extern int xv6_open(const char *path, int flags);
extern int xv6_read(int fd, void *buf, u32 size);
extern int xv6_write(int fd, const void *buf, u32 size);
extern int xv6_close(int fd);

#define O_RDONLY 0x0000

int main(int argc, char **argv)
{
  int i;
  char buf[128];

  if(argc < 2){
    printf("usage: cat /path...\n");
    return 1;
  }

  for(i = 1; i < argc; i++){
    int fd = xv6_open(argv[i], O_RDONLY);
    if(fd < 0){
      printf("cat: failed: %s\n", argv[i]);
      return 1;
    }
    while(1){
      int n = xv6_read(fd, buf, sizeof(buf));
      if(n < 0){
        printf("cat: read failed: %s\n", argv[i]);
        xv6_close(fd);
        return 1;
      }
      if(n == 0)
        break;
      if(xv6_write(1, buf, (u32)n) != n){
        xv6_close(fd);
        return 1;
      }
    }
    xv6_close(fd);
  }

  return 0;
}
