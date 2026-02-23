/**
 * @file hal.h
 * @brief Hardware Abstraction Layer interface
 *
 * Platform-agnostic interface for hardware-dependent operations.
 * Implementations are provided by platform-specific files (e.g., hal_esp32s3.c).
 */
#ifndef XV6_HAL_H
#define XV6_HAL_H

#include "core/types.h"

/**
 * @brief Initialize console subsystem
 */
void hal_console_init(void);

/**
 * @brief Get character from console
 * @return Character read, or -1 if no input
 */
int hal_console_getc(void);

/**
 * @brief Put character to console
 * @param c Character to output
 */
void hal_console_putc(int c);

/**
 * @brief Initialize timer subsystem
 */
void hal_timer_init(void);

/**
 * @brief Get timer ticks since boot
 * @return Number of ticks
 */
uint64 hal_ticks(void);

/**
 * @brief Delay for specified milliseconds
 * @param ms Milliseconds to delay
 */
void hal_delay_ms(uint32 ms);

/**
 * @brief Reboot the system
 */
void hal_reboot(void);

/**
 * @brief Get free heap memory
 * @return Free heap bytes
 */
uint64 hal_free_heap_bytes(void);

#endif
