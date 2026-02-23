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
 * Converts e.g. Ctrl+A to value 1
 */
#define C(x)  ((x)-'@')

/**
 * @brief Console input buffer structure.
 */
struct {
  struct spinlock lock;  // Protects the buffer
  
  // input circular buffer
#define INPUT_BUF_SIZE 128
  char buf[INPUT_BUF_SIZE];  // Ring buffer for input characters
  uint r;  // Read index - where consoleread reads from
  uint w;  // Write index - where consoleintr writes to (line complete)
  uint e;  // Edit index - where consoleintr writes to (current line)
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
    // If user typed backspace, overwrite last char with space, then backspace again
    // This visually erases the character on terminal
    uartputc_sync('\b'); // Move cursor back
    uartputc_sync(' '); // Write space to overwrite character
    uartputc_sync('\b'); // Move cursor back again
  } else {
    // Normal character - just output it
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
  char buf[32]; // Local buffer for batch copying
  int i = 0;    // Bytes processed

  // Process in batches to reduce copyin calls
  while(i < n){
    // Determine batch size (min of buffer size and remaining bytes)
    int nn = sizeof(buf);
    if(nn > n - i)
      nn = n - i;
    
    // Copy batch from user/kernel memory
    if(either_copyin(buf, user_src, src+i, nn) == -1)
      break;  // Stop on error
      
    // Write batch to UART
    uartwrite(buf, nn);
    i += nn;
  }

  return i;  // Return bytes written
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
  int target;  // Original requested byte count
  int c;        // Character being processed
  char cbuf;    // Temporary buffer for single character

  target = n;
  
  // Acquire lock to safely access buffer
  acquire(&cons.lock);
  
  // Loop until we have data or can return
  while(n > 0){
    // Wait until there's data in buffer
    // (write index != read index means buffer has data)
    while(cons.r == cons.w){
      // Check if current process was killed - if so, exit gracefully
      if(killed(myproc())){
        release(&cons.lock);
        return -1;
      }
      // Sleep waiting for input - releases lock while waiting
      sleep(&cons.r, &cons.lock);
    }

    // Get next character from buffer
    c = cons.buf[cons.r++ % INPUT_BUF_SIZE];  // Advance read index

    // Handle EOF (Ctrl+D)
    if(c == C('D')){  // end-of-file
      if(n < target){
        // Not at start of buffer - put the ^D back for next read
        cons.r--;
      }
      break;  // Return what we have
    }

    // Copy character to user buffer
    cbuf = c;
    if(either_copyout(user_dst, dst, &cbuf, 1) == -1){
      if(n == target)  // Failed on first char - return error
        n = -1;
      break;
    }

    dst++;  // Advance destination
    --n;    // Decrement remaining count

    // Handle newline - line is complete
    if(c == '\n'){
      break;  // Return the line
    }
  }
  release(&cons.lock);

  // Return error or bytes read
  if(n < 0)
    return -1;
  return target - n;  // Bytes successfully read
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

  // Handle special control characters
  switch(c){
  case C('P'):  // Print process list (Ctrl+P)
    procdump();
    break;
  case C('U'):  // Kill line (Ctrl+U)
    // Delete characters back to beginning of line
    while(cons.e != cons.w &&
          cons.buf[(cons.e-1) % INPUT_BUF_SIZE] != '\n'){
      cons.e--;
      consputc(BACKSPACE);  // Visually erase
    }
    break;
  case C('H'): // Backspace (Ctrl+H)
  case '\x7f': // Delete key
    // Delete one character if available
    if(cons.e != cons.w){
      cons.e--;
      consputc(BACKSPACE);
    }
    break;
  default:
    // Regular character - only accept if buffer has room
    if(c != 0 && cons.e-cons.r < INPUT_BUF_SIZE){
      // Convert carriage return to newline
      c = (c == '\r') ? '\n' : c;

      // Echo character back to user (so they can see what they typed)
      consputc(c);

      // Store in buffer for consumption
      cons.buf[cons.e++ % INPUT_BUF_SIZE] = c;

      // If line is complete (newline, EOF, or buffer full), wake reader
      if(c == '\n' || c == C('D') || cons.e-cons.r == INPUT_BUF_SIZE){
        // Mark write position - reader can now read this line
        cons.w = cons.e;
        // Wake up any waiting readers
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
  // Initialize the console lock
  initlock(&cons.lock, "cons");

  // Initialize UART hardware
  uartinit();

  // Register console with device switch table
  // so read/write system calls go to our functions
  devsw[CONSOLE].read = consoleread;
  devsw[CONSOLE].write = consolewrite;
}
