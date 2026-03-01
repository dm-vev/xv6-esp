#include "../kernel/platform/xv6_esp_boot.h"
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
  if(xv6_network_runtime_init() != 0){
    xv6_boot();
    return;
  }

  if(xTaskCreatePinnedToCore(xv6_boot_task, "xv6_boot", 24576, 0, tskIDLE_PRIORITY + 2, 0, 0) != pdPASS)
    xv6_boot();
}
