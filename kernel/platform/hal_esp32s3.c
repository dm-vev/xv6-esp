#include "platform/hal.h"

#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hal/uart_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * @file hal_esp32s3.c
 * @brief Hardware Abstraction Layer for ESP32-S3
 *
 * Provides platform-specific implementations for:
 * - Console I/O (UART/USB)
 * - Time and delays
 * - Memory management
 * - System control
 */

#define XV6_CONSOLE_PUSHBACK_CAP 128
static uint8 g_console_pushback[XV6_CONSOLE_PUSHBACK_CAP];
static uint16 g_console_pushback_r;
static uint16 g_console_pushback_w;
static uint16 g_console_pushback_n;

static int hal_console_pop_pushback(void)
{
  int out;
  if(g_console_pushback_n == 0)
    return -1;
  out = (int)g_console_pushback[g_console_pushback_r];
  g_console_pushback_r = (uint16)((g_console_pushback_r + 1u) % XV6_CONSOLE_PUSHBACK_CAP);
  g_console_pushback_n--;
  return out;
}

static void hal_console_pushback_byte(uint8 c)
{
  if(g_console_pushback_n >= XV6_CONSOLE_PUSHBACK_CAP)
    return;
  g_console_pushback[g_console_pushback_w] = c;
  g_console_pushback_w = (uint16)((g_console_pushback_w + 1u) % XV6_CONSOLE_PUSHBACK_CAP);
  g_console_pushback_n++;
}

static int hal_console_getc_hw(void)
{
  uart_dev_t *hw = UART_LL_GET_HW(0);
  uint8 ch = 0;
  if(uart_ll_get_rxfifo_len(hw) > 0){
    uart_ll_read_rxfifo(hw, &ch, 1);
    return (int)ch;
  }

  return -1;
}

void hal_console_init(void)
{
  g_console_pushback_r = 0;
  g_console_pushback_w = 0;
  g_console_pushback_n = 0;
}

int hal_console_getc(void)
{
  int c = hal_console_pop_pushback();
  if(c >= 0)
    return c;
  return hal_console_getc_hw();
}

int hal_console_poll_ctrl_c(void)
{
  int c = hal_console_getc_hw();
  if(c < 0)
    return 0;
  if(c == 0x03)
    return 1;
  hal_console_pushback_byte((uint8)c);
  return 0;
}

void hal_console_putc(int c)
{
  uart_dev_t *hw = UART_LL_GET_HW(0);
  const uint8 ch = (uint8)c;
  while(uart_ll_get_txfifo_len(hw) == 0){
  }
  uart_ll_write_txfifo(hw, &ch, 1);
}

void hal_timer_init(void)
{
}

uint64 hal_ticks(void)
{
  return (uint64)(esp_timer_get_time() / 10000);
}

void hal_delay_ms(uint32 ms)
{
  TickType_t ticks = pdMS_TO_TICKS(ms);
  if(ms > 0 && ticks == 0)
    ticks = 1;
  if(ticks > 0)
    vTaskDelay(ticks);
  else
    taskYIELD();
}

void hal_reboot(void)
{
  esp_restart();
}

uint64 hal_free_heap_bytes(void)
{
  return (uint64)heap_caps_get_free_size(MALLOC_CAP_8BIT);
}
