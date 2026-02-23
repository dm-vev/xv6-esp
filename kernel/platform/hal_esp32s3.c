#include "platform/hal.h"

#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "driver/uart.h"
#if CONFIG_USJ_ENABLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define XV6_CONSOLE_UART UART_NUM_0
#define XV6_CONSOLE_RX_BUF_SIZE 256
#define XV6_CONSOLE_TX_BUF_SIZE 1024
#if CONFIG_USJ_ENABLE_USB_SERIAL_JTAG
static int g_usb_serial_jtag_ready;
#endif

void hal_console_init(void)
{
  const uart_config_t cfg = {
    .baud_rate = 115200,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .source_clk = UART_SCLK_DEFAULT,
  };

  (void)uart_driver_install(XV6_CONSOLE_UART, XV6_CONSOLE_RX_BUF_SIZE, XV6_CONSOLE_TX_BUF_SIZE, 0, 0, 0);
  (void)uart_param_config(XV6_CONSOLE_UART, &cfg);
  (void)uart_set_pin(XV6_CONSOLE_UART, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

#if CONFIG_USJ_ENABLE_USB_SERIAL_JTAG
  if(!usb_serial_jtag_is_driver_installed()){
    usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if(usb_serial_jtag_driver_install(&usj_cfg) == ESP_OK)
      g_usb_serial_jtag_ready = 1;
  } else {
    g_usb_serial_jtag_ready = 1;
  }
#endif
}

int hal_console_getc(void)
{
  uint8 ch = 0;
  size_t uart_avail = 0;

  if(uart_get_buffered_data_len(XV6_CONSOLE_UART, &uart_avail) == ESP_OK && uart_avail > 0){
    if(uart_read_bytes(XV6_CONSOLE_UART, &ch, 1, 0) == 1)
      return (int)ch;
  }

#if CONFIG_USJ_ENABLE_USB_SERIAL_JTAG
  if(g_usb_serial_jtag_ready && usb_serial_jtag_is_connected()){
    if(usb_serial_jtag_read_bytes(&ch, 1, 0) == 1)
      return (int)ch;
  }
#endif

  return -1;
}

void hal_console_putc(int c)
{
  const uint8 ch = (uint8)c;
  (void)uart_write_bytes(XV6_CONSOLE_UART, (const char *)&ch, 1);
#if CONFIG_USJ_ENABLE_USB_SERIAL_JTAG
  if(g_usb_serial_jtag_ready && usb_serial_jtag_is_connected())
    (void)usb_serial_jtag_write_bytes(&ch, 1, 0);
#endif
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
