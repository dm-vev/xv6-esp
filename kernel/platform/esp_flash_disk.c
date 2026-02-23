#include "platform/esp_flash_disk.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/**
 * @file esp_flash_disk.c
 * @brief ESP32 flash-based disk implementation
 *
 * Implements a block device interface using ESP32's flash partition.
 * Provides read/write/erase operations for the xv6 filesystem
 * using the ESP-IDF partition API.
 */

static const char *TAG = "xv6_flash_disk";
static const char *XV6_PARTITION_LABEL = "xv6fs";
static const int XV6_FLASH_WRITE_RETRY = 2;
#define XV6_FLASH_ERASE_SIZE 4096U

static const esp_partition_t *g_part;
static SemaphoreHandle_t g_disk_mu;
static uint32 g_num_sectors;
static uint8 g_rmw_scratch[XV6_FLASH_ERASE_SIZE];
static uint8 g_verify_scratch[XV6_FLASH_ERASE_SIZE];

static int flash_commit_chunk_locked(uint32 blk_base, uint32 wr_start, uint32 wr_end, const uint8 *src, uint32 src_off)
{
  esp_err_t err = ESP_OK;
  uint32 copy_off;
  uint32 copy_len;
  int attempt;

  if(wr_start < blk_base || wr_end < wr_start || wr_end > (blk_base + XV6_FLASH_ERASE_SIZE))
    return -1;

  copy_off = wr_start - blk_base;
  copy_len = wr_end - wr_start;

  for(attempt = 0; attempt < XV6_FLASH_WRITE_RETRY; attempt++){
    err = esp_partition_read(g_part, blk_base, g_rmw_scratch, XV6_FLASH_ERASE_SIZE);
    if(err != ESP_OK)
      continue;

    memcpy(g_rmw_scratch + copy_off, src + src_off, copy_len);

    err = esp_partition_erase_range(g_part, blk_base, XV6_FLASH_ERASE_SIZE);
    if(err != ESP_OK)
      continue;

    err = esp_partition_write(g_part, blk_base, g_rmw_scratch, XV6_FLASH_ERASE_SIZE);
    if(err != ESP_OK)
      continue;

    err = esp_partition_read(g_part, wr_start, g_verify_scratch, copy_len);
    if(err != ESP_OK)
      continue;

    if(memcmp(g_verify_scratch, g_rmw_scratch + copy_off, copy_len) == 0)
      return 0;
  }

  return -1;
}

int esp_flash_disk_init(void)
{
  g_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                    XV6_PARTITION_LABEL);
  if(g_part == 0){
    ESP_LOGE(TAG, "partition '%s' not found", XV6_PARTITION_LABEL);
    return -1;
  }

  if((g_part->size % XV6_FLASH_SECTOR_SIZE) != 0){
    ESP_LOGE(TAG, "partition size is not aligned to %u bytes", XV6_FLASH_SECTOR_SIZE);
    return -1;
  }

  g_num_sectors = (uint32)(g_part->size / XV6_FLASH_SECTOR_SIZE);
  g_disk_mu = xSemaphoreCreateMutex();
  if(g_disk_mu == 0){
    ESP_LOGE(TAG, "mutex allocation failed");
    return -1;
  }

  ESP_LOGI(TAG, "partition=%s offset=0x%lx size=%lu sectors=%lu",
           XV6_PARTITION_LABEL,
           (unsigned long)g_part->address,
           (unsigned long)g_part->size,
           (unsigned long)g_num_sectors);
  return 0;
}

uint32 esp_flash_disk_num_sectors(void)
{
  return g_num_sectors;
}

int esp_flash_disk_read(uint32 sector, void *dst, uint32 sector_count)
{
  esp_err_t err;
  uint32 off;
  uint32 len;

  if(g_part == 0 || dst == 0)
    return -1;
  if(sector_count == 0)
    return 0;
  if(sector >= g_num_sectors || sector_count > (g_num_sectors - sector))
    return -1;

  off = sector * XV6_FLASH_SECTOR_SIZE;
  len = sector_count * XV6_FLASH_SECTOR_SIZE;

  if(xSemaphoreTake(g_disk_mu, portMAX_DELAY) != pdTRUE)
    return -1;

  err = esp_partition_read(g_part, off, dst, len);
  xSemaphoreGive(g_disk_mu);
  return (err == ESP_OK) ? 0 : -1;
}

int esp_flash_disk_write(uint32 sector, const void *src, uint32 sector_count)
{
  uint32 off;
  uint32 len;
  uint32 pos;
  const uint8 *in;

  if(g_part == 0 || src == 0)
    return -1;
  if(sector_count == 0)
    return 0;
  if(sector >= g_num_sectors || sector_count > (g_num_sectors - sector))
    return -1;
  off = sector * XV6_FLASH_SECTOR_SIZE;
  len = sector_count * XV6_FLASH_SECTOR_SIZE;
  in = (const uint8 *)src;

  if(xSemaphoreTake(g_disk_mu, portMAX_DELAY) != pdTRUE)
    return -1;

  pos = off;
  while(pos < off + len){
    uint32 blk_base = (pos / XV6_FLASH_ERASE_SIZE) * XV6_FLASH_ERASE_SIZE;
    uint32 blk_end = blk_base + XV6_FLASH_ERASE_SIZE;
    uint32 wr_start = pos;
    uint32 wr_end = off + len;

    if(wr_end > blk_end)
      wr_end = blk_end;

    if(flash_commit_chunk_locked(blk_base, wr_start, wr_end, in, wr_start - off) != 0){
      xSemaphoreGive(g_disk_mu);
      return -1;
    }

    pos = wr_end;
  }

  xSemaphoreGive(g_disk_mu);
  return 0;
}
