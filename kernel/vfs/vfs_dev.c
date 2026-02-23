/**
 * @file vfs_dev.c
 * @brief Device file operations implementation
 *
 * Implements character device access for /dev/ filesystem entries.
 * Supports various standard devices:
 * - /dev/null: infinite sink, returns EOF on read, discards on write
 * - /dev/zero: returns zeros, accepts any write
 * - /dev/full: returns zeros, rejects all writes
 * - /dev/random, /dev/urandom: pseudo-random number generator
 * - /dev/stdin, /dev/tty: console input
 * - /dev/console, /dev/stdout, /dev/stderr, /dev/kmsg: console output
 */
#include "vfs/vfs_dev.h"

#include <string.h>

#include "core/types.h"
#include "platform/hal.h"

#define MAXPATH 256

/**
 * @brief Canonicalize device path
 *
 * Converts various path aliases to their canonical form:
 * - /dev/fd/0 -> /dev/stdin
 * - /dev/fd/1 -> /dev/stdout
 * - /dev/fd/2 -> /dev/stderr
 * - /dev/pts/ptmx -> /dev/ptmx
 *
 * @param path     Original path
 * @param[out] out Buffer for canonical path
 * @param out_len Buffer size
 * @return 0 on success, -1 on error
 */
static int dev_canonical_path(const char *path, char *out, int out_len)
{
  if(path == 0 || out == 0 || out_len <= 0)
    return -1;
  
  /* Handle /dev/fd/N aliases */
  if(strcmp(path, "/dev/fd/0") == 0)
    path = "/dev/stdin";
  else if(strcmp(path, "/dev/fd/1") == 0)
    path = "/dev/stdout";
  else if(strcmp(path, "/dev/fd/2") == 0)
    path = "/dev/stderr";
  else if(strcmp(path, "/dev/pts/ptmx") == 0)
    path = "/dev/ptmx";
  
  /* Check buffer can hold the path */
  if((int)strlen(path) >= out_len)
    return -1;
  
  strcpy(out, path);
  return 0;
}

/**
 * @brief Fill buffer with pseudo-random bytes
 *
 * Generates pseudo-random bytes using a simple LCG (Linear Congruential
 * Generator). Not cryptographically secure - use for non-security
 * purposes only.
 *
 * @param[out] buf Buffer to fill
 * @param n      Number of bytes
 * @return Number of bytes generated
 */
static int dev_prng_fill(void *buf, uint32 n)
{
  uint8 *p = (uint8 *)buf;
  uint32 x = (uint32)hal_ticks() ^ 0x9e3779b9u;  /* Seed from ticks + magic constant */
  uint32 i;

  /* Simple LCG: xorshift algorithm */
  for(i = 0; i < n; i++){
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    p[i] = (uint8)x;
  }
  return (int)n;
}

int vfs_dev_init(void)
{
  /* No initialization needed - device table is static */
  return 0;
}

int vfs_dev_is_device(const char *path)
{
  char canon[MAXPATH];
  
  /* Canonicalize and check if path is under /dev/ */
  if(dev_canonical_path(path, canon, sizeof(canon)) != 0)
    return 0;
    
  return strncmp(canon, "/dev/", 5) == 0 || strcmp(canon, "/dev") == 0;
}

int vfs_dev_read(const char *path, uint32 off, void *buf, uint32 size)
{
  char canon[MAXPATH];
  uint8 *p = (uint8 *)buf;
  uint32 i = 0;

  /* Validate and canonicalize path */
  if(dev_canonical_path(path, canon, sizeof(canon)) != 0 || buf == 0)
    return -1;
    
  (void)off;  /* Character devices don't use offset */

  /* /dev/null: always returns EOF (0 bytes) */
  if(strcmp(canon, "/dev/null") == 0)
    return 0;
    
  /* /dev/zero and /dev/full: return zeros */
  if(strcmp(canon, "/dev/zero") == 0 || strcmp(canon, "/dev/full") == 0){
    memset(buf, 0, size);
    return (int)size;
  }
  
  /* /dev/random and /dev/urandom: return pseudo-random data */
  if(strcmp(canon, "/dev/random") == 0 || strcmp(canon, "/dev/urandom") == 0)
    return dev_prng_fill(buf, size);
  
  /* /dev/stdin and /dev/tty: read from console */
  if(strcmp(canon, "/dev/stdin") == 0 || strcmp(canon, "/dev/tty") == 0){
    /* Read characters until buffer full, newline, or no data available */
    while(i < size){
      int c = hal_console_getc();
      if(c < 0){
        /* No data available */
        if(i > 0)
          break;
        /* Wait a bit for data */
        hal_delay_ms(1);
        continue;
      }
      p[i++] = (uint8)c;
      /* Stop on line ending */
      if(c == '\n' || c == '\r')
        break;
    }
    return (int)i;
  }

  /* Unknown device */
  return -1;
}

int vfs_dev_write(const char *path, const void *data, uint32 size)
{
  char canon[MAXPATH];
  const char *c = (const char *)data;
  uint32 left = size;

  /* Validate and canonicalize path */
  if(dev_canonical_path(path, canon, sizeof(canon)) != 0 || data == 0)
    return -1;
    
  /* These devices don't support writing */
  if(strcmp(canon, "/dev/full") == 0 || strcmp(canon, "/dev/stdin") == 0)
    return -1;

  /* Console output devices: write each character to UART */
  if(strcmp(canon, "/dev/console") == 0 || strcmp(canon, "/dev/tty") == 0 || 
     strcmp(canon, "/dev/stdout") == 0 || strcmp(canon, "/dev/stderr") == 0 || 
     strcmp(canon, "/dev/kmsg") == 0){
    while(left--)
      hal_console_putc(*c++);
    return (int)size;
  }

  /* Sink devices: accept all data but don't do anything */
  if(strcmp(canon, "/dev/null") == 0 || strcmp(canon, "/dev/zero") == 0 || 
     strcmp(canon, "/dev/random") == 0 || strcmp(canon, "/dev/urandom") == 0)
    return (int)size;
    
  /* Unknown device */
  return -1;
}
