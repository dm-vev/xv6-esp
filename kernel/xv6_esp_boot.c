#include "xv6_esp_boot.h"

#include "esp_log.h"

#include "esp_flash_disk.h"
#include "hal.h"
#include "ksh.h"
#include "xv6fs_ro.h"

static const char *TAG = "xv6_boot";

void xv6_boot(void)
{
  hal_console_init();
  hal_timer_init();

  ESP_LOGI(TAG, "xv6 ESP32-S3 M1 boot");
  ESP_LOGI(TAG, "init flash disk backend");
  if(esp_flash_disk_init() != 0){
    ESP_LOGE(TAG, "flash disk init failed");
  }
  if(xv6fs_ro_init() != 0)
    ESP_LOGE(TAG, "xv6fs mount failed");

  ksh_run();
}
