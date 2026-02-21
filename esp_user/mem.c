#include "xv6_user.h"

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  printf("free_heap=%d bytes\n", k_free_heap());
  return 0;
}
