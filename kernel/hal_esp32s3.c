#include "hal.h"

#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define XV6_CONSOLE_UART UART_NUM_0
#define XV6_CONSOLE_BUF_SIZE 256

static volatile uint64 g_ticks;
static esp_timer_handle_t g_tick_timer;

static void tick_timer_cb(void *arg)
{
  (void)arg;
  g_ticks++;
}

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

  (void)uart_driver_install(XV6_CONSOLE_UART, XV6_CONSOLE_BUF_SIZE, 0, 0, 0, 0);
  (void)uart_param_config(XV6_CONSOLE_UART, &cfg);
  (void)uart_set_pin(XV6_CONSOLE_UART, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

int hal_console_getc(void)
{
  uint8 ch = 0;
  int n = uart_read_bytes(XV6_CONSOLE_UART, &ch, 1, pdMS_TO_TICKS(20));
  if(n == 1)
    return (int)ch;
  return -1;
}

void hal_console_putc(int c)
{
  const uint8 ch = (uint8)c;
  (void)uart_write_bytes(XV6_CONSOLE_UART, (const char *)&ch, 1);
}

void hal_timer_init(void)
{
  const esp_timer_create_args_t args = {
    .callback = tick_timer_cb,
    .name = "xv6_tick",
  };
  (void)esp_timer_create(&args, &g_tick_timer);
  (void)esp_timer_start_periodic(g_tick_timer, 10000); // 10 ms
}

uint64 hal_ticks(void)
{
  return g_ticks;
}

void hal_delay_ms(uint32 ms)
{
  vTaskDelay(pdMS_TO_TICKS(ms));
}

void hal_reboot(void)
{
  esp_restart();
}

uint64 hal_free_heap_bytes(void)
{
  return (uint64)heap_caps_get_free_size(MALLOC_CAP_8BIT);
}
