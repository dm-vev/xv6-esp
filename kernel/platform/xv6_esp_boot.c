#include "platform/xv6_esp_boot.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_log.h"
#include "sdkconfig.h"

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

#ifndef CONFIG_IDF_TARGET
#define CONFIG_IDF_TARGET "esp32"
#endif

#ifndef CONFIG_IDF_TARGET_ARCH
#define CONFIG_IDF_TARGET_ARCH "unknown"
#endif

static void log_stage(const char *stage)
{
  ESP_LOGI(TAG, "stage=%s ticks=%llu free_heap=%llu", stage, (unsigned long long)hal_ticks(),
           (unsigned long long)hal_free_heap_bytes());
}

static void log_memory_caps(const char *stage)
{
  size_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  size_t psram_size = 0;
  int psram_init = 0;

#if CONFIG_SPIRAM
  psram_init = esp_psram_is_initialized() ? 1 : 0;
  if(psram_init)
    psram_size = esp_psram_get_size();
#endif

  ESP_LOGI(TAG,
           "memory stage=%s internal_total=%llu internal_free=%llu internal_largest=%llu psram_init=%d "
           "psram_size=%llu psram_heap_total=%llu psram_free=%llu psram_largest=%llu",
           stage, (unsigned long long)internal_total, (unsigned long long)internal_free,
           (unsigned long long)internal_largest, psram_init, (unsigned long long)psram_size,
           (unsigned long long)psram_total, (unsigned long long)psram_free, (unsigned long long)psram_largest);
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

  ESP_LOGI(TAG, "xv6 %s %s boot", CONFIG_IDF_TARGET, CONFIG_IDF_TARGET_ARCH);
  log_memory_caps("boot_start");
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
  log_memory_caps("shell_runtime_init_ok");
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
