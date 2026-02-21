typedef unsigned int u32;

extern int printf(const char *fmt, ...);
extern void free(void *p);
extern int xv6fs_read_file_alloc_path(const char *path, void **out_data, u32 *out_size);

int main(int argc, char **argv)
{
  int i;

  if(argc < 2){
    printf("usage: cat /path...\n");
    return 1;
  }

  for(i = 1; i < argc; i++){
    void *data = 0;
    u32 size = 0;
    if(xv6fs_read_file_alloc_path(argv[i], &data, &size) != 0){
      printf("cat: failed: %s\n", argv[i]);
      return 1;
    }
    if(size > 0)
      printf("%.*s", (int)size, (const char *)data);
    free(data);
  }

  return 0;
}
