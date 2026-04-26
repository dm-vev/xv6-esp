#include <errno.h>
#include <stdio.h>

#include "xv6_user.h"

#define PSRAM_EXPECTED_TOTAL (32 * 1024 * 1024)

static int parse_size(const char *s, int *out)
{
  unsigned long value = 0;
  unsigned long mul = 1;

  if(s == 0 || *s == 0 || out == 0)
    return -1;
  while(*s >= '0' && *s <= '9'){
    value = (value * 10ul) + (unsigned long)(*s - '0');
    if(value > 64ul * 1024ul * 1024ul)
      return -1;
    s++;
  }
  if(*s == 'k' || *s == 'K'){
    mul = 1024ul;
    s++;
  } else if(*s == 'm' || *s == 'M'){
    mul = 1024ul * 1024ul;
    s++;
  }
  if(*s != 0 || value == 0)
    return -1;
  if(value > (unsigned long)0x7fffffff / mul)
    return -1;
  *out = (int)(value * mul);
  return 0;
}

static int run_one(int bytes)
{
  int before;
  int after;

  before = k_psram_free();
  if(k_psram_test(bytes) != 0){
    printf("psram_test: fail size=%d errno=%d\n", bytes, errno);
    return 1;
  }
  after = k_psram_free();
  printf("psram_test: ok size=%d free_before=%d free_after=%d\n", bytes, before, after);
  return 0;
}

int main(int argc, char **argv)
{
  int total = k_psram_total();
  int free_bytes = k_psram_free();
  int largest = k_psram_largest();
  int i;
  int rc = 0;

  printf("psram_test: total=%d free=%d largest=%d\n", total, free_bytes, largest);
  if(total < PSRAM_EXPECTED_TOTAL){
    printf("psram_test: expected at least %d bytes\n", PSRAM_EXPECTED_TOTAL);
    return 2;
  }

  if(argc <= 1){
    int medium = 1024 * 1024;
    int large = 8 * 1024 * 1024;

    if(run_one(1) != 0)
      rc = 1;
    if(run_one(4096) != 0)
      rc = 1;
    if(largest >= medium && run_one(medium) != 0)
      rc = 1;
    if(largest >= large && run_one(large) != 0)
      rc = 1;
    return rc;
  }

  for(i = 1; i < argc; i++){
    int bytes;
    if(parse_size(argv[i], &bytes) != 0){
      fprintf(stderr, "usage: psram_test [bytes|Nk|Nm ...]\n");
      return 1;
    }
    if(run_one(bytes) != 0)
      rc = 1;
  }

  return rc;
}
