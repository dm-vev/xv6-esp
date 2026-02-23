/**
 * @file printf.c
 * @brief Formatted console output implementation.
 *
 * Provides printf and panic functions for formatted output to console.
 */

#include <stdarg.h>

#include "core/types.h"
#include "core/param.h"
#include "core/spinlock.h"
#include "core/sleeplock.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "core/memlayout.h"
#include "arch/riscv.h"
#include "core/defs.h"
#include "core/proc.h"

/**
 * @brief Indicates if system is currently panicking.
 */
volatile int panicking = 0;

/**
 * @brief Indicates if panic has occurred.
 *
 * When set, causes spinning indefinitely at end of panic.
 */
volatile int panicked = 0;

/**
 * @brief Printf lock to avoid interleaving output.
 */
static struct {
  struct spinlock lock;
} pr;

/**
 * @brief Character lookup table for numeric conversion.
 */
static char digits[] = "0123456789abcdef";

/**
 * @brief Prints a signed or unsigned integer in the given base.
 *
 * @param xx   Value to print.
 * @param base Radix (10 for decimal, 16 for hex, etc.).
 * @param sign If non-zero, treat as signed integer.
 *
 * @post Number printed to console in specified base.
 *
 * @note Handles negative numbers for signed conversion.
 */
static void
printint(long long xx, int base, int sign)
{
  char buf[20];
  int i;
  unsigned long long x;

  if(sign && (sign = (xx < 0)))
    x = -xx;
  else
    x = xx;

  i = 0;
  do {
    buf[i++] = digits[x % base];
  } while((x /= base) != 0);

  if(sign)
    buf[i++] = '-';

  while(--i >= 0)
    consputc(buf[i]);
}

/**
 * @brief Prints a pointer value in hex format.
 *
 * @param x Pointer value to print.
 *
 * @post Pointer printed as "0x" followed by hex digits.
 */
static void
printptr(uint64 x)
{
  int i;
  consputc('0');
  consputc('x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    consputc(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

/**
 * @brief Prints formatted output to the console.
 *
 * Supports format specifiers:
 * - %d, %ld, %lld: signed decimal
 * - %u, %lu, %llu: unsigned decimal
 * - %x, %lx, %llx: hexadecimal
 * - %p: pointer
 * - %c: character
 * - %s: string
 * - %%: literal percent sign
 *
 * @param fmt Format string.
 * @param ... Arguments for format specifiers.
 *
 * @post Formatted output written to console.
 *
 * @return Always returns 0.
 */
int
printf(char *fmt, ...)
{
  va_list ap;
  int i, cx, c0, c1, c2;
  char *s;

  if(panicking == 0)
    acquire(&pr.lock);

  va_start(ap, fmt);
  for(i = 0; (cx = fmt[i] & 0xff) != 0; i++){
    if(cx != '%'){
      consputc(cx);
      continue;
    }
    i++;
    c0 = fmt[i+0] & 0xff;
    c1 = c2 = 0;
    if(c0) c1 = fmt[i+1] & 0xff;
    if(c1) c2 = fmt[i+2] & 0xff;
    if(c0 == 'd'){
      printint(va_arg(ap, int), 10, 1);
    } else if(c0 == 'l' && c1 == 'd'){
      printint(va_arg(ap, uint64), 10, 1);
      i += 1;
    } else if(c0 == 'l' && c1 == 'l' && c2 == 'd'){
      printint(va_arg(ap, uint64), 10, 1);
      i += 2;
    } else if(c0 == 'u'){
      printint(va_arg(ap, uint32), 10, 0);
    } else if(c0 == 'l' && c1 == 'u'){
      printint(va_arg(ap, uint64), 10, 0);
      i += 1;
    } else if(c0 == 'l' && c1 == 'l' && c2 == 'u'){
      printint(va_arg(ap, uint64), 10, 0);
      i += 2;
    } else if(c0 == 'x'){
      printint(va_arg(ap, uint32), 16, 0);
    } else if(c0 == 'l' && c1 == 'x'){
      printint(va_arg(ap, uint64), 16, 0);
      i += 1;
    } else if(c0 == 'l' && c1 == 'l' && c2 == 'x'){
      printint(va_arg(ap, uint64), 16, 0);
      i += 2;
    } else if(c0 == 'p'){
      printptr(va_arg(ap, uint64));
    } else if(c0 == 'c'){
      consputc(va_arg(ap, uint));
    } else if(c0 == 's'){
      if((s = va_arg(ap, char*)) == 0)
        s = "(null)";
      for(; *s; s++)
        consputc(*s);
    } else if(c0 == '%'){
      consputc('%');
    } else if(c0 == 0){
      break;
    } else {
      // Print unknown % sequence to draw attention.
      consputc('%');
      consputc(c0);
    }

  }
  va_end(ap);

  if(panicking == 0)
    release(&pr.lock);

  return 0;
}

/**
 * @brief Prints a panic message and halts.
 *
 * @param s Panic message string.
 *
 * @post Panic message printed.
 * @post panicking flag set to 1.
 * @post panicked flag set to 1.
 *
 * @note Does not return - enters infinite loop.
 *
 * @return None.
 */
void
panic(char *s)
{
  panicking = 1;
  printf("panic: ");
  printf("%s\n", s);
  panicked = 1; // freeze uart output from other CPUs
  for(;;)
    ;
}

/**
 * @brief Initializes the printf subsystem.
 *
 * @post Printf lock is initialized.
 *
 * @return None.
 */
void
printfinit(void)
{
  initlock(&pr.lock, "pr");
}
