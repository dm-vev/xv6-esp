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
 * @brief Poll console for a specific byte without consuming other bytes
 * @param target Byte value to detect (0..255)
 * @return 1 if target byte was received, 0 otherwise
 */
int hal_console_poll_byte(int target);

/**
 * @brief Poll console for Ctrl+C without consuming other bytes
 * @return 1 if Ctrl+C was received, 0 otherwise
 */
int hal_console_poll_ctrl_c(void);

/**
 * @brief Poll console for Ctrl+Z without consuming other bytes
 * @return 1 if Ctrl+Z was received, 0 otherwise
 */
int hal_console_poll_ctrl_z(void);

int hal_console_has_input(void);

/**
 * @brief Discard any queued console input bytes
 */
void hal_console_discard_input(void);

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

/**
 * @brief Get total heap memory capacity
 * @return Total heap bytes
 */
uint64 hal_total_heap_bytes(void);

#endif
