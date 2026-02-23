/**
 * @file string.c
 * @brief String and memory manipulation functions.
 */

#include "core/types.h"

/**
 * @brief Fills a block of memory with a byte value.
 *
 * @param dst Destination memory block.
 * @param c   Byte value to fill with.
 * @param n   Number of bytes to fill.
 *
 * @post dst filled with n copies of byte c.
 *
 * @return Pointer to dst.
 */
void*
memset(void *dst, int c, uint n)
{
  char *cdst = (char *) dst;
  int i;
  for(i = 0; i < n; i++){
    cdst[i] = c;
  }
  return dst;
}

/**
 * @brief Compares two memory blocks.
 *
 * @param v1 First memory block.
 * @param v2 Second memory block.
 * @param n  Number of bytes to compare.
 *
 * @return 0 if blocks are equal.
 * @return Negative if v1 < v2.
 * @return Positive if v1 > v2.
 */
int
memcmp(const void *v1, const void *v2, uint n)
{
  const uchar *s1, *s2;

  s1 = v1;
  s2 = v2;
  while(n-- > 0){
    if(*s1 != *s2)
      return *s1 - *s2;
    s1++, s2++;
  }

  return 0;
}

/**
 * @brief Copies a block of memory.
 *
 * Handles overlapping regions correctly by copying
 * from end to start when regions overlap.
 *
 * @param dst Destination memory block.
 * @param src Source memory block.
 * @param n   Number of bytes to copy.
 *
 * @post src copied to dst.
 *
 * @return Pointer to dst.
 */
void*
memmove(void *dst, const void *src, uint n)
{
  const char *s;
  char *d;

  if(n == 0)
    return dst;
  
  s = src;
  d = dst;
  if(s < d && s + n > d){
    s += n;
    d += n;
    while(n-- > 0)
      *--d = *--s;
  } else
    while(n-- > 0)
      *d++ = *s++;

  return dst;
}

/**
 * @brief Copies a block of memory.
 *
 * @param dst Destination memory block.
 * @param src Source memory block.
 * @param n   Number of bytes to copy.
 *
 * @note Uses memmove internally to handle overlapping regions.
 *
 * @return Pointer to dst.
 */
void*
memcpy(void *dst, const void *src, uint n)
{
  return memmove(dst, src, n);
}

/**
 * @brief Compares two strings up to n characters.
 *
 * @param p First string.
 * @param q Second string.
 * @param n Maximum characters to compare.
 *
 * @return 0 if strings are equal up to n characters.
 * @return Difference between differing characters.
 */
int
strncmp(const char *p, const char *q, uint n)
{
  while(n > 0 && *p && *p == *q)
    n--, p++, q++;
  if(n == 0)
    return 0;
  return (uchar)*p - (uchar)*q;
}

/**
 * @brief Copies a string (not null-terminated).
 *
 * @param s Destination buffer.
 * @param t Source string.
 * @param n Maximum characters to copy.
 *
 * @post Exactly n bytes copied (may not be null-terminated).
 *
 * @return Pointer to destination buffer.
 */
char*
strncpy(char *s, const char *t, int n)
{
  char *os;

  os = s;
  while(n-- > 0 && (*s++ = *t++) != 0)
    ;
  while(n-- > 0)
    *s++ = 0;
  return os;
}

/**
 * @brief Copies a string, always null-terminated.
 *
 * @param s Destination buffer.
 * @param t Source string.
 * @param n Size of destination buffer.
 *
 * @post String copied and always null-terminated.
 *
 * @return Pointer to destination buffer.
 */
char*
safestrcpy(char *s, const char *t, int n)
{
  char *os;

  os = s;
  if(n <= 0)
    return os;
  while(--n > 0 && (*s++ = *t++) != 0)
    ;
  *s = 0;
  return os;
}

/**
 * @brief Returns the length of a string.
 *
 * @param s Null-terminated string.
 *
 * @return Number of characters before null terminator.
 */
int
strlen(const char *s)
{
  int n;

  for(n = 0; s[n]; n++)
    ;
  return n;
}
