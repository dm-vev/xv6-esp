/**
 * @file esp_flash_disk.h
 * @brief ESP32 flash disk interface
 *
 * Block device interface for reading/writing to ESP32 flash partition.
 */
#ifndef XV6_ESP_FLASH_DISK_H
#define XV6_ESP_FLASH_DISK_H

#include "core/types.h"

/**
 * @brief Flash sector size in bytes
 */
#define XV6_FLASH_SECTOR_SIZE 512U

/**
 * @brief Initialize flash disk
 * @return 0 on success, -1 on failure
 */
int esp_flash_disk_init(void);

/**
 * @brief Read sectors from flash
 * @param sector Starting sector number
 * @param dst Destination buffer
 * @param sector_count Number of sectors to read
 * @return 0 on success, -1 on failure
 */
int esp_flash_disk_read(uint32 sector, void *dst, uint32 sector_count);

/**
 * @brief Write sectors to flash
 * @param sector Starting sector number
 * @param src Source buffer
 * @param sector_count Number of sectors to write
 * @return 0 on success, -1 on failure
 */
int esp_flash_disk_write(uint32 sector, const void *src, uint32 sector_count);

/**
 * @brief Get number of sectors
 * @return Total number of sectors available
 */
uint32 esp_flash_disk_num_sectors(void);

#endif
