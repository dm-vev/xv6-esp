typedef unsigned int u32;
typedef unsigned short u16;

extern int printf(const char *fmt, ...);
extern int xv6fs_readdir_path(const char *path, int index, char *name_out, int name_out_len, u16 *type_out,
                              u32 *size_out);

int main(int argc, char **argv)
{
  int idx = 0;
  char name[15];
  const char *path = (argc > 1) ? argv[1] : "/";
  u16 type = 0;
  u32 size = 0;

  while(xv6fs_readdir_path(path, idx, name, sizeof(name), &type, &size) == 0){
    if(type == 1)
      printf("d %s\n", name);
    else
      printf("- %s\t%u\n", name, (unsigned)size);
    idx++;
  }
  return 0;
}
