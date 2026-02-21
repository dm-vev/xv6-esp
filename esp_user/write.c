extern int printf(const char *fmt, ...);
extern unsigned int strlen(const char *s);
extern void *memcpy(void *dst, const void *src, unsigned int n);
extern int xv6fs_write_file_path(const char *path, const void *data, unsigned int size);

int main(int argc, char **argv)
{
  char buf[256];
  unsigned int n = 0;
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

  if(xv6fs_write_file_path(argv[1], buf, n) != 0){
    printf("write: failed: %s\n", argv[1]);
    return 1;
  }
  return 0;
}
