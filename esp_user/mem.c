extern int printf(const char *fmt, ...);
extern int k_free_heap(void);

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  printf("free_heap=%d bytes\n", k_free_heap());
  return 0;
}
