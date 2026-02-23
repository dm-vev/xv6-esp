/**
 * @file console.c
 * @brief Console input and output implementation.
 *
 * Handles UART-based console I/O with line buffering and
 * special character processing:
 *   newline -- end of line
 *   control-h -- backspace
 *   control-u -- kill line
 *   control-d -- end of file
 *   control-p -- print process list
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
 * @brief Backspace character code - erases last output character.
 */
#define BACKSPACE 0x100

/**
 * @brief Converts control character to control code.
 */
#define C(x)  ((x)-'@')

/**
 * @brief Console input buffer structure.
 */
struct {
  struct spinlock lock;
  
  // input circular buffer
#define INPUT_BUF_SIZE 128
  char buf[INPUT_BUF_SIZE];
  uint r;  // Read index
  uint w;  // Write index
  uint e;  // Edit index
} cons;

/**
 * @brief Outputs a single character to UART synchronously.
 *
 * Does not use interrupts or sleep - safe to call from interrupt
 * handlers (e.g., by printf or to echo input characters).
 *
 * @param c Character to output.
 *
 * @note Handles BACKSPACE by printing backspace-space-backspace sequence.
 *
 * @return None.
 */
void
consputc(int c)
{
  if(c == BACKSPACE){
    // if the user typed backspace, overwrite with a space.
    uartputc_sync('\b'); uartputc_sync(' '); uartputc_sync('\b');
  } else {
    uartputc_sync(c);
  }
}

/**
 * @brief Handles user write() system calls to the console.
 *
 * Copies data from user or kernel space and writes to UART.
 *
 * @param user_src If non-zero, src is user address; else kernel address.
 * @param src      Source address to copy from.
 * @param n        Number of bytes to write.
 *
 * @post Data written to UART.
 *
 * @return Number of bytes actually written.
 */
int
consolewrite(int user_src, uint64 src, int n)
{
  char buf[32]; // move batches from user space to uart.
  int i = 0;

  while(i < n){
    int nn = sizeof(buf);
    if(nn > n - i)
      nn = n - i;
    if(either_copyin(buf, user_src, src+i, nn) == -1)
      break;
    uartwrite(buf, nn);
    i += nn;
  }

  return i;
}

/**
 * @brief Handles user read() system calls from the console.
 *
 * Copies a whole input line to the destination buffer.
 * Blocks until a line is available or EOF.
 *
 * @param user_dst If non-zero, dst is user address; else kernel address.
 * @param dst      Destination buffer address.
 * @param n        Maximum bytes to read.
 *
 * @post Line copied to destination buffer.
 *
 * @return Number of bytes read, -1 on error.
 *
 * @error Returns -1 if process was killed while waiting.
 * @error Returns -1 if copyout fails.
 */
int
consoleread(int user_dst, uint64 dst, int n)
{
  int target;
  int c;
  char cbuf;

  target = n;
  acquire(&cons.lock);
  while(n > 0){
    // wait until interrupt handler has put some
    // input into cons.buffer.
    while(cons.r == cons.w){
      if(killed(myproc())){
        release(&cons.lock);
        return -1;
      }
      sleep(&cons.r, &cons.lock);
    }

    c = cons.buf[cons.r++ % INPUT_BUF_SIZE];

    if(c == C('D')){  // end-of-file
      if(n < target){
        // Save ^D for next time, to make sure
        // caller gets a 0-byte result.
        cons.r--;
      }
      break;
    }

    // copy the input byte to the user-space buffer.
    cbuf = c;
    if(either_copyout(user_dst, dst, &cbuf, 1) == -1){
      if(n == target)
        n = -1;
      break;
    }

    dst++;
    --n;

    if(c == '\n'){
      // a whole line has arrived, return to
      // the user-level read().
      break;
    }
  }
  release(&cons.lock);

  if(n < 0)
    return -1;
  return target - n;
}

/**
 * @brief Console input interrupt handler.
 *
 * Called by uartintr() for each input character.
 * Handles special characters and appends to console buffer.
 * Wakes up consoleread() when a whole line arrives.
 *
 * @param c Input character from UART.
 *
 * @pre Console lock must be held.
 *
 * @post Input processed and added to buffer.
 * @post consoleread() woken up if line complete.
 *
 * @note Handles: ^P (procdump), ^U (kill line), ^H/del (backspace).
 *
 * @return None.
 */
void
consoleintr(int c)
{
  acquire(&cons.lock);

  switch(c){
  case C('P'):  // Print process list.
    procdump();
    break;
  case C('U'):  // Kill line.
    while(cons.e != cons.w &&
          cons.buf[(cons.e-1) % INPUT_BUF_SIZE] != '\n'){
      cons.e--;
      consputc(BACKSPACE);
    }
    break;
  case C('H'): // Backspace
  case '\x7f': // Delete key
    if(cons.e != cons.w){
      cons.e--;
      consputc(BACKSPACE);
    }
    break;
  default:
    if(c != 0 && cons.e-cons.r < INPUT_BUF_SIZE){
      c = (c == '\r') ? '\n' : c;

      // echo back to the user.
      consputc(c);

      // store for consumption by consoleread().
      cons.buf[cons.e++ % INPUT_BUF_SIZE] = c;

      if(c == '\n' || c == C('D') || cons.e-cons.r == INPUT_BUF_SIZE){
        // wake up consoleread() if a whole line (or end-of-file)
        // has arrived.
        cons.w = cons.e;
        wakeup(&cons.r);
      }
    }
    break;
  }
  
  release(&cons.lock);
}

/**
 * @brief Initializes the console subsystem.
 *
 * Sets up console lock and UART, then registers
 * console read/write handlers with device switch table.
 *
 * @post Console lock initialized.
 * @post UART initialized.
 * @post Console registered as character device.
 *
 * @return None.
 */
void
consoleinit(void)
{
  initlock(&cons.lock, "cons");

  uartinit();

  // connect read and write system calls
  // to consoleread and consolewrite.
  devsw[CONSOLE].read = consoleread;
  devsw[CONSOLE].write = consolewrite;
}
