#ifndef XV6_HAL_H
#define XV6_HAL_H

#include "types.h"

void hal_console_init(void);
int hal_console_getc(void);
void hal_console_putc(int c);

void hal_timer_init(void);
uint64 hal_ticks(void);

void hal_delay_ms(uint32 ms);
void hal_reboot(void);
uint64 hal_free_heap_bytes(void);

#endif
