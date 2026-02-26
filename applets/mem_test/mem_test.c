#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int g_failures = 0;
static int g_tests = 0;

#define MALLOC_MANY_BLOCK_SIZE 1024
#define MALLOC_MANY_MAX_BLOCKS  50
#define MALLOC_MANY_MIN_BLOCKS  32

static void check(const char *name, int ok)
{
  g_tests++;
  if(!ok){
    printf("FAIL %s\n", name);
    g_failures++;
  } else {
    printf("PASS %s\n", name);
  }
}

static void check_eq(const char *name, int a, int b)
{
  g_tests++;
  if(a != b){
    printf("FAIL %s: got %d, expected %d\n", name, a, b);
    g_failures++;
  } else {
    printf("PASS %s\n", name);
  }
}

int main(void)
{
  void *p;
  void *tmp;
  void *blocks[MALLOC_MANY_MAX_BLOCKS];
  char *s;
  int i;

  printf("=== Memory Allocator Tests ===\n");

  p = malloc(0);
  check("malloc_zero", 1);
  if(p)
    free(p);

  p = malloc(1);
  check("malloc_small", p != 0);
  free(p);

  p = malloc(4096);
  check("malloc_page", p != 0);
  free(p);

  p = malloc(100);
  check_eq("malloc_alignment", (int)((uintptr_t)p % sizeof(void *)), 0);
  free(p);

  p = calloc(10, 10);
  check("calloc_basic", p != 0);
  if(p){
    char *cp = (char *)p;
    int i;
    for(i = 0; i < 100; i++){
      if(cp[i] != 0){
        break;
      }
    }
    check_eq("calloc_zeroed", i, 100);
    free(p);
  }

  p = realloc(0, 100);
  check("realloc_null", p != 0);
  free(p);

  p = malloc(50);
  if(p){
    memset(p, 0xAA, 50);
    tmp = realloc(p, 100);
    check("realloc_grow", tmp != 0);
    if(!tmp){
      free(p);
      p = 0;
    } else {
      p = tmp;
      char *cp = (char *)p;
      int i;
      for(i = 0; i < 50; i++){
        if(cp[i] != 0xAA){
          break;
        }
      }
      check_eq("realloc_preserved", i, 50);
    }
    free(p);
  }

  p = malloc(100);
  if(p){
    memset(p, 0xBB, 100);
    tmp = realloc(p, 50);
    check("realloc_shrink", tmp != 0);
    if(!tmp){
      free(p);
      p = 0;
    } else {
      p = tmp;
      char *cp = (char *)p;
      int i;
      for(i = 0; i < 50; i++){
        if(cp[i] != 0xBB){
          break;
        }
      }
      check_eq("realloc_shrink_data", i, 50);
    }
    free(p);
  }

  s = strdup("hello");
  check("strdup_basic", s != 0);
  if(s){
    check_eq("strdup_content", strcmp(s, "hello"), 0);
    free(s);
  }

  s = strndup("world", 3);
  check("strndup_basic", s != 0);
  if(s){
    check_eq("strndup_content", strncmp(s, "wor", 3), 0);
    check_eq("strndup_nul", s[3], '\0');
    free(s);
  }

  memset(blocks, 0, sizeof(blocks));
  for(i = 0; i < MALLOC_MANY_MAX_BLOCKS; i++){
    blocks[i] = malloc(MALLOC_MANY_BLOCK_SIZE);
    if(!blocks[i])
      break;
  }
  check("malloc_many_capacity", i >= MALLOC_MANY_MIN_BLOCKS);
  while(i > 0){
    i--;
    free(blocks[i]);
  }

  free(0);

  printf("=== Mem Test: %d/%d passed ===\n", g_tests - g_failures, g_tests);
  return (g_failures == 0) ? 0 : 1;
}
