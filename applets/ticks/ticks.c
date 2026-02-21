#include <stdio.h>

extern int k_ticks(void);

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  printf("ticks=%d\n", k_ticks());
  return 0;
}
