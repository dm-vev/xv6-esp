/**
 * @file uart.c
 * @brief Low-level UART driver implementation.
 *
 * On ESP builds, routes xv6 console I/O through hal_console_* to avoid
 * relying on 16550 registers and TX interrupts that do not exist there.
 */

#ifdef ESP_PLATFORM

#include "core/types.h"
#include "platform/hal.h"

/**
 * @brief Initializes UART (ESP platform - no-op).
 *
 * @return None.
 */
void
uartinit(void)
{
}

/**
 * @brief Writes data to UART (ESP platform).
 *
 * @param buf Buffer containing data to write.
 * @param n   Number of bytes to write.
 *
 * @post Data written to console via HAL.
 *
 * @return None.
 */
void
uartwrite(char buf[], int n)
{
  for(int i = 0; i < n; i++)
    hal_console_putc((uint8)buf[i]);
}

/**
 * @brief Writes a character synchronously (ESP platform).
 *
 * @param c Character to write.
 *
 * @return None.
 */
void
uartputc_sync(int c)
{
  hal_console_putc(c);
}

/**
 * @brief Reads a character from UART (ESP platform).
 *
 * @return Character read, or -1 if none available.
 */
int
uartgetc(void)
{
  return hal_console_getc();
}

/**
 * @brief UART interrupt handler (ESP platform - no-op).
 *
 * @return None.
 */
void
uartintr(void)
{
}

#else

/**
 * @file uart.c
 * @brief Low-level driver for 16550a UART.
 */

#include "core/types.h"
#include "core/param.h"
#include "core/memlayout.h"
#include "arch/riscv.h"
#include "core/spinlock.h"
#include "core/proc.h"
#include "core/defs.h"

// the UART control registers are memory-mapped
// at address UART0. this macro returns the
// address of one of the registers.
#define Reg(reg) ((volatile unsigned char *)(UART0 + (reg)))

#define ReadReg(reg) (*(Reg(reg)))
#define WriteReg(reg, v) (*(Reg(reg)) = (v))

// the UART control registers.
// some have different meanings for read vs write.
// see http://byterunner.com/16550.html
#define RHR 0                 // receive holding register (for input bytes);
#define THR 0                 // transmit holding register (for output bytes);
#define IER 1                 // interrupt enable register
#define IER_RX_ENABLE (1<<0)
#define IER_TX_ENABLE (1<<1)
#define FCR 2                 // FIFO control register
#define FCR_FIFO_ENABLE (1<<0)
#define FCR_FIFO_CLEAR (3<<1) // clear the content of the two FIFOs
#define ISR 2                 // interrupt status register
#define LCR 3                 // line control register
#define LCR_EIGHT_BITS (3<<0)
#define LCR_BAUD_LATCH (1<<7) // special mode to set baud rate
#define LSR 5                 // line status register
#define LSR_RX_READY (1<<0)   // input is waiting to be read from RHR
#define LSR_TX_IDLE (1<<5)    // THR can accept another character to send

// for sending threads to synchronize with uart "ready" interrupts.
/** @brief Lock protecting UART transmission. */
static struct spinlock tx_lock;
/** @brief Whether UART is currently transmitting. */
static int tx_busy;
/** @brief Address used as sleep channel for tx. */
static int tx_chan;

extern volatile int panicking; // from printf.c
extern volatile int panicked; // from printf.c

/**
 * @brief Initializes the UART hardware.
 *
 * Configures UART for 38400 baud, 8N1, enables FIFO,
 * and enables transmit/receive interrupts.
 *
 * @post UART initialized and interrupts enabled.
 *
 * @return None.
 */
void
uartinit(void)
{
  // disable interrupts.
  WriteReg(IER, 0x00);

  // special mode to set baud rate.
  WriteReg(LCR, LCR_BAUD_LATCH);

  // LSB for baud rate of 38.4K.
  WriteReg(0, 0x03);

  // MSB for baud rate of 38.4K.
  WriteReg(1, 0x00);

  // leave set-baud mode,
  // and set word length to 8 bits, no parity.
  WriteReg(LCR, LCR_EIGHT_BITS);

  // reset and enable FIFOs.
  WriteReg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);

  // enable transmit and receive interrupts.
  WriteReg(IER, IER_TX_ENABLE | IER_RX_ENABLE);

  initlock(&tx_lock, "uart");
}

/**
 * @brief Writes data to UART.
 *
 * @param buf Buffer containing data to write.
 * @param n   Number of bytes to write.
 *
 * @post Data transmitted to UART.
 *
 * @return None.
 *
 * @note Blocks if UART is busy. Cannot be called from interrupts.
 */
void
uartwrite(char buf[], int n)
{
  acquire(&tx_lock);

  int i = 0;
  while(i < n){ 
    while(tx_busy != 0){
      // wait for a UART transmit-complete interrupt
      // to set tx_busy to 0.
      sleep(&tx_chan, &tx_lock);
    }
      
    WriteReg(THR, buf[i]);
    i += 1;
    tx_busy = 1;
  }

  release(&tx_lock);
}


/**
 * @brief Writes a character to UART synchronously.
 *
 * @param c Character to write.
 *
 * @post Character transmitted to UART.
 *
 * @note Spins waiting for UART to be ready. Used by kernel printf.
 * @note Disables interrupts while sending.
 *
 * @return None.
 */
void
uartputc_sync(int c)
{
  if(panicking == 0)
    push_off();

  if(panicked){
    for(;;)
      ;
  }

  // wait for UART to set Transmit Holding Empty in LSR.
  while((ReadReg(LSR) & LSR_TX_IDLE) == 0)
    ;
  WriteReg(THR, c);

  if(panicking == 0)
    pop_off();
}

/**
 * @brief Reads a character from UART if available.
 *
 * @return Character read (0-255), or -1 if none waiting.
 */
int
uartgetc(void)
{
  if(ReadReg(LSR) & LSR_RX_READY){
    // input data is ready.
    return ReadReg(RHR);
  } else {
    return -1;
  }
}

/**
 * @brief UART interrupt handler.
 *
 * Handles transmit-complete and receive-ready interrupts.
 *
 * @post Transmit thread woken if transmission complete.
 * @post Incoming characters processed via consoleintr().
 *
 * @return None.
 */
void
uartintr(void)
{
  ReadReg(ISR); // acknowledge the interrupt

  acquire(&tx_lock);
  if(ReadReg(LSR) & LSR_TX_IDLE){
    // UART finished transmitting; wake up sending thread.
    tx_busy = 0;
    wakeup(&tx_chan);
  }
  release(&tx_lock);

  // read and process incoming characters, if any.
  while(1){
    int c = uartgetc();
    if(c == -1)
      break;
    consoleintr(c);
  }
}

#endif
