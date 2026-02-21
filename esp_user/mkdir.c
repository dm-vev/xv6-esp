extern int printf(const char *fmt, ...);
extern int xv6fs_mkdir_path(const char *path);

int main(int argc, char **argv)
{
  int i;
  if(argc < 2){
    printf("usage: mkdir /path\n");
    return 1;
  }
  for(i = 1; i < argc; i++){
    if(xv6fs_mkdir_path(argv[i]) != 0){
      printf("mkdir: failed: %s\n", argv[i]);
      return 1;
    }
  }
  return 0;
}
