#include "../kernel/xv6_esp_boot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void xv6_boot_task(void *arg)
{
  (void)arg;
  xv6_boot();
  vTaskDelete(NULL);
}

void app_main(void)
{
  if(xTaskCreatePinnedToCore(xv6_boot_task, "xv6_boot", 12288, 0, tskIDLE_PRIORITY + 2, 0, 0) != pdPASS)
    xv6_boot();
}
