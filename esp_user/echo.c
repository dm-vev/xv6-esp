#include "xv6_user.h"

int main(int argc, char **argv)
{
  int i;
  for(i = 1; i < argc; i++){
    u32 n = strlen(argv[i]);
    if(n > 0 && xv6_write(1, argv[i], n) != (int)n)
      return 1;
    if(i + 1 < argc && xv6_write(1, " ", 1) != 1)
      return 1;
  }
  if(xv6_write(1, "\n", 1) != 1)
    return 1;
  return 0;
}
