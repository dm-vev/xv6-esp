#ifndef XV6_ESP_FLASH_DISK_H
#define XV6_ESP_FLASH_DISK_H

#include "core/types.h"

#define XV6_FLASH_SECTOR_SIZE 512U

int esp_flash_disk_init(void);
int esp_flash_disk_read(uint32 sector, void *dst, uint32 sector_count);
int esp_flash_disk_write(uint32 sector, const void *src, uint32 sector_count);
uint32 esp_flash_disk_num_sectors(void);

#endif
