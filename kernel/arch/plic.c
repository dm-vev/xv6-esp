/**
 * @file plic.c
 * @brief RISC-V Platform Level Interrupt Controller (PLIC) implementation.
 */

#include "core/types.h"
#include "core/param.h"
#include "core/memlayout.h"
#include "arch/riscv.h"
#include "core/defs.h"

/**
 * @brief Initializes the PLIC.
 *
 * Sets up interrupt priorities for UART and Virtio devices.
 * Priority must be non-zero for interrupts to be enabled.
 *
 * @post UART and Virtio interrupts have priority 1.
 *
 * @return None.
 */
void
plicinit(void)
{
  // set desired IRQ priorities non-zero (otherwise disabled).
  *(uint32*)(PLIC + UART0_IRQ*4) = 1;
  *(uint32*)(PLIC + VIRTIO0_IRQ*4) = 1;
}

/**
 * @brief Initializes PLIC for the current hart.
 *
 * Enables UART and Virtio interrupts for this CPU and
 * sets priority threshold to 0 (accept all interrupts).
 *
 * @post UART and Virtio interrupts enabled for this hart.
 * @post Priority threshold set to 0.
 *
 * @return None.
 */
void
plicinithart(void)
{
  int hart = cpuid();
  
  // set enable bits for this hart's S-mode
  // for the uart and virtio disk.
  *(uint32*)PLIC_SENABLE(hart) = (1 << UART0_IRQ) | (1 << VIRTIO0_IRQ);

  // set this hart's S-mode priority threshold to 0.
  *(uint32*)PLIC_SPRIORITY(hart) = 0;
}

/**
 * @brief Claims a pending interrupt from PLIC.
 *
 * Reads the claim register to get the IRQ number of
 * the highest priority pending interrupt.
 *
 * @return IRQ number of the interrupt to handle.
 */
int
plic_claim(void)
{
  int hart = cpuid();
  int irq = *(uint32*)PLIC_SCLAIM(hart);
  return irq;
}

/**
 * @brief Signals completion of interrupt handling.
 *
 * Writes the IRQ number back to the claim register
 * to tell the PLIC the interrupt has been handled.
 *
 * @param irq IRQ number that was claimed.
 *
 * @return None.
 */
void
plic_complete(int irq)
{
  int hart = cpuid();
  *(uint32*)PLIC_SCLAIM(hart) = irq;
}
