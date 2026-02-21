#include "xv6_user.h"

int main(int argc, char **argv)
{
  int idx = 0;
  char name[15];
  const char *path = (argc > 1) ? argv[1] : ".";
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
