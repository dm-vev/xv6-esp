#include <stdio.h>

extern int xv6fs_readdir_path(const char *path, int index, char *name_out, int name_out_len,
                              unsigned short *type_out, unsigned int *size_out);

static int list_path(const char *path)
{
  int idx = 0;
  int printed = 0;
  char name[64];
  unsigned short type = 0;
  unsigned int size = 0;

  for(;;){
    int rc = xv6fs_readdir_path(path, idx, name, (int)sizeof(name), &type, &size);
    if(rc < 0){
      if(idx == 0){
        fprintf(stderr, "ls: cannot access '%s'\n", path);
        return 1;
      }
      break;
    }
    if(rc == 1)
      break;
    puts(name);
    printed = 1;
    idx++;
  }

  return printed ? 0 : 0;
}

int main(int argc, char **argv)
{
  int i;
  int rc = 0;

  if(argc <= 1)
    return list_path(".");

  for(i = 1; i < argc; i++){
    int one = list_path(argv[i]);
    if(one != 0)
      rc = one;
  }
  return rc;
}
