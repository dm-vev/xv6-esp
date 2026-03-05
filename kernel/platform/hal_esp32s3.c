#include "platform/hal.h"

#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hal/uart_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

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
static int g_usb_console_driver_ready;
static portMUX_TYPE g_console_pushback_mu = portMUX_INITIALIZER_UNLOCKED;

static int hal_console_pop_pushback_locked(void)
{
  int out;
  if(g_console_pushback_n == 0)
    return -1;
  out = (int)g_console_pushback[g_console_pushback_r];
  g_console_pushback_r = (uint16)((g_console_pushback_r + 1u) % XV6_CONSOLE_PUSHBACK_CAP);
  g_console_pushback_n--;
  return out;
}

static int hal_console_peek_pushback_locked(void)
{
  if(g_console_pushback_n == 0)
    return -1;
  return (int)g_console_pushback[g_console_pushback_r];
}

static void hal_console_pushback_byte_locked(uint8 c)
{
  if(g_console_pushback_n >= XV6_CONSOLE_PUSHBACK_CAP)
    return;
  g_console_pushback[g_console_pushback_w] = c;
  g_console_pushback_w = (uint16)((g_console_pushback_w + 1u) % XV6_CONSOLE_PUSHBACK_CAP);
  g_console_pushback_n++;
}

static void hal_console_clear_pushback_locked(void)
{
  g_console_pushback_r = 0;
  g_console_pushback_w = 0;
  g_console_pushback_n = 0;
}

static int hal_console_pop_pushback(void)
{
  int out;
  portENTER_CRITICAL(&g_console_pushback_mu);
  out = hal_console_pop_pushback_locked();
  portEXIT_CRITICAL(&g_console_pushback_mu);
  return out;
}

static void hal_console_pushback_byte(uint8 c)
{
  portENTER_CRITICAL(&g_console_pushback_mu);
  hal_console_pushback_byte_locked(c);
  portEXIT_CRITICAL(&g_console_pushback_mu);
}

static int hal_console_getc_uart(void)
{
  uart_dev_t *hw = UART_LL_GET_HW(0);
  uint8 ch = 0;
  if(uart_ll_get_rxfifo_len(hw) > 0){
    uart_ll_read_rxfifo(hw, &ch, 1);
    return (int)ch;
  }

  return -1;
}

static void hal_console_putc_uart(uint8 ch)
{
  uart_dev_t *hw = UART_LL_GET_HW(0);
  while(uart_ll_get_txfifo_len(hw) == 0){
  }
  uart_ll_write_txfifo(hw, &ch, 1);
}

#if SOC_USB_SERIAL_JTAG_SUPPORTED
static int hal_console_usb_connected(void)
{
  if(!g_usb_console_driver_ready)
    return 0;
  return usb_serial_jtag_is_connected() ? 1 : 0;
}

static int hal_console_getc_usb(void)
{
  uint8 ch = 0;
  int n = usb_serial_jtag_read_bytes(&ch, 1, 0);
  if(n > 0)
    return (int)ch;
  return -1;
}

static void hal_console_putc_usb(uint8 ch)
{
  (void)usb_serial_jtag_write_bytes(&ch, 1, 0);
}
#else
static int hal_console_usb_connected(void)
{
  return 0;
}

static int hal_console_getc_usb(void)
{
  return -1;
}

static void hal_console_putc_usb(uint8 ch)
{
  (void)ch;
}
#endif

static int hal_console_getc_hw(void)
{
  if(hal_console_usb_connected()){
    int c = hal_console_getc_usb();
    if(c >= 0)
      return c;
    return -1;
  }

  return hal_console_getc_uart();
}

static void hal_console_putc_hw(uint8 ch)
{
  if(hal_console_usb_connected()){
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    hal_console_putc_usb(ch);
#endif
    return;
  }

  hal_console_putc_uart(ch);
}

static void hal_console_init_usb(void)
{
#if SOC_USB_SERIAL_JTAG_SUPPORTED
  if(usb_serial_jtag_is_driver_installed()){
    g_usb_console_driver_ready = 1;
    return;
  }

  {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if(usb_serial_jtag_driver_install(&cfg) == ESP_OK)
      g_usb_console_driver_ready = 1;
  }
#endif
}

void hal_console_init(void)
{
  g_console_pushback_r = 0;
  g_console_pushback_w = 0;
  g_console_pushback_n = 0;
  g_usb_console_driver_ready = 0;
  hal_console_init_usb();
}

int hal_console_getc(void)
{
  int c = hal_console_pop_pushback();
  if(c >= 0)
    return c;
  return hal_console_getc_hw();
}

int hal_console_poll_byte(int target)
{
  int queued;
  int c;
  if(target < 0 || target > 0xff)
    return 0;
  portENTER_CRITICAL(&g_console_pushback_mu);
  queued = hal_console_peek_pushback_locked();
  if(queued == target){
    (void)hal_console_pop_pushback_locked();
    portEXIT_CRITICAL(&g_console_pushback_mu);
    return 1;
  }
  portEXIT_CRITICAL(&g_console_pushback_mu);

  c = hal_console_getc_hw();
  if(c < 0)
    return 0;
  if(c == target)
    return 1;
  hal_console_pushback_byte((uint8)c);
  return 0;
}

int hal_console_poll_ctrl_c(void)
{
  return hal_console_poll_byte(0x03);
}

int hal_console_poll_ctrl_z(void)
{
  return hal_console_poll_byte(0x1a);
}

void hal_console_discard_input(void)
{
  portENTER_CRITICAL(&g_console_pushback_mu);
  hal_console_clear_pushback_locked();
  portEXIT_CRITICAL(&g_console_pushback_mu);

  while(hal_console_getc_hw() >= 0){
  }
}

void hal_console_putc(int c)
{
  const uint8 ch = (uint8)c;
  hal_console_putc_hw(ch);
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
  return (uint64)heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
}

uint64 hal_total_heap_bytes(void)
{
  return (uint64)heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
}
