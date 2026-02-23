#include "xv6_esp_boot.h"

#include "esp_log.h"

#include "esp_flash_disk.h"
#include "hal.h"
#include "shell_runtime.h"
#include "xv6fs_ro.h"

static const char *TAG = "xv6_boot";

static void log_stage(const char *stage)
{
  ESP_LOGI(TAG, "stage=%s ticks=%llu free_heap=%llu", stage, (unsigned long long)hal_ticks(),
           (unsigned long long)hal_free_heap_bytes());
}

void xv6_boot(void)
{
  uint64 boot_start_ticks;
  int rc;

  hal_console_init();
  hal_timer_init();
  boot_start_ticks = hal_ticks();

  ESP_LOGI(TAG, "xv6 ESP32-S3 M1 boot");
  log_stage("boot_start");
  ESP_LOGI(TAG, "init flash disk backend");
  rc = esp_flash_disk_init();
  if(rc != 0){
    ESP_LOGE(TAG, "flash disk init failed");
    log_stage("flash_disk_init_failed");
    hal_reboot();
    return;
  }
  log_stage("flash_disk_init_ok");
  rc = xv6fs_ro_init();
  if(rc != 0){
    ESP_LOGE(TAG, "xv6fs mount failed");
    log_stage("xv6fs_mount_failed");
    hal_reboot();
    return;
  }
  log_stage("xv6fs_mount_ok");

  rc = shell_runtime_init();
  if(rc != 0){
    ESP_LOGE(TAG, "shell runtime init failed");
    log_stage("shell_runtime_init_failed");
    hal_reboot();
    return;
  }
  log_stage("shell_runtime_init_ok");
  rc = shell_runtime_bootstrap("/bin/sh");
  if(rc != 0){
    ESP_LOGE(TAG, "failed to bootstrap /bin/sh");
    log_stage("shell_bootstrap_failed");
    hal_reboot();
    return;
  }
  ESP_LOGI(TAG, "shell bootstrap returned unexpectedly duration_ms=%llu",
           (unsigned long long)((hal_ticks() - boot_start_ticks) * 10ull));
  log_stage("shell_bootstrap_returned");
  hal_reboot();
}
