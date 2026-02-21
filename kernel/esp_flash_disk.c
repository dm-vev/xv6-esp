#include "esp_flash_disk.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "xv6_flash_disk";
static const char *XV6_PARTITION_LABEL = "xv6fs";
static const uint32 XV6_FLASH_ERASE_SIZE = 4096U;

static const esp_partition_t *g_part;
static SemaphoreHandle_t g_disk_mu;
static uint32 g_num_sectors;

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
  esp_err_t err = ESP_OK;
  uint32 off;
  uint32 len;
  uint32 pos;
  const uint8 *in;
  const uint32 erase_sz = XV6_FLASH_ERASE_SIZE;
  uint8 *scratch = 0;

  if(g_part == 0 || src == 0)
    return -1;
  if(sector_count == 0)
    return 0;
  if(sector >= g_num_sectors || sector_count > (g_num_sectors - sector))
    return -1;
  if(erase_sz == 0)
    return -1;

  off = sector * XV6_FLASH_SECTOR_SIZE;
  len = sector_count * XV6_FLASH_SECTOR_SIZE;
  in = (const uint8 *)src;

  if(xSemaphoreTake(g_disk_mu, portMAX_DELAY) != pdTRUE)
    return -1;

  scratch = malloc(erase_sz);
  if(scratch == 0){
    xSemaphoreGive(g_disk_mu);
    return -1;
  }

  pos = off;
  while(pos < off + len && err == ESP_OK){
    uint32 blk_base = (pos / erase_sz) * erase_sz;
    uint32 blk_end = blk_base + erase_sz;
    uint32 wr_start = pos;
    uint32 wr_end = off + len;
    uint32 copy_len;
    uint32 copy_off;

    if(wr_end > blk_end)
      wr_end = blk_end;
    copy_off = wr_start - blk_base;
    copy_len = wr_end - wr_start;

    err = esp_partition_read(g_part, blk_base, scratch, erase_sz);
    if(err != ESP_OK)
      break;
    memcpy(scratch + copy_off, in + (wr_start - off), copy_len);
    err = esp_partition_erase_range(g_part, blk_base, erase_sz);
    if(err != ESP_OK)
      break;
    err = esp_partition_write(g_part, blk_base, scratch, erase_sz);
    pos = wr_end;
  }

  free(scratch);
  xSemaphoreGive(g_disk_mu);
  return (err == ESP_OK) ? 0 : -1;
}
