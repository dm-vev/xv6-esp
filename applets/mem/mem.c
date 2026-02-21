#include <stdio.h>

extern int k_free_heap(void);

int main(void)
{
  printf("free_heap=%d bytes\n", k_free_heap());
  return 0;
}
