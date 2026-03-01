#include "platform/xv6_esp_boot.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

#include "platform/esp_flash_disk.h"
#include "platform/hal.h"
#include "runtime/shell_runtime.h"
#include "vfs/vfs.h"

/**
 * @file xv6_esp_boot.c
 * @brief xv6 ESP32 boot and initialization
 *
 * This file handles the xv6 kernel initialization on ESP32 platform.
 * It coordinates loading the filesystem image, initializing VFS,
 * loading kernel modules, and starting the shell.
 */

static const char *TAG = "xv6_boot";

static void log_stage(const char *stage)
{
  ESP_LOGI(TAG, "stage=%s ticks=%llu free_heap=%llu", stage, (unsigned long long)hal_ticks(),
           (unsigned long long)hal_free_heap_bytes());
}

int xv6_network_runtime_init(void)
{
  esp_err_t err;

  err = esp_netif_init();
  if(err != ESP_OK && err != ESP_ERR_INVALID_STATE){
    ESP_LOGE(TAG, "esp_netif_init failed err=0x%x", (unsigned)err);
    return -1;
  }

  err = esp_event_loop_create_default();
  if(err != ESP_OK && err != ESP_ERR_INVALID_STATE){
    ESP_LOGE(TAG, "esp_event_loop_create_default failed err=0x%x", (unsigned)err);
    return -1;
  }

  return 0;
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
  rc = xv6_network_runtime_init();
  if(rc != 0){
    ESP_LOGE(TAG, "network runtime init failed");
    log_stage("network_runtime_init_failed");
    hal_reboot();
    return;
  }
  log_stage("network_runtime_init_ok");
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
  rc = shell_runtime_bootstrap("/bin/init");
  if(rc != 0){
    ESP_LOGE(TAG, "failed to bootstrap /bin/init");
    log_stage("init_bootstrap_failed");
    hal_reboot();
    return;
  }
  ESP_LOGI(TAG, "init returned unexpectedly duration_ms=%llu",
           (unsigned long long)((hal_ticks() - boot_start_ticks) * 10ull));
  log_stage("init_returned");
  hal_reboot();
}
