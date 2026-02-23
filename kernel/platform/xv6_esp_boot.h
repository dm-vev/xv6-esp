/**
 * @file xv6_esp_boot.h
 * @brief xv6 ESP32 boot interface
 */
#ifndef XV6_ESP_BOOT_H
#define XV6_ESP_BOOT_H

/**
 * @brief Main boot function for xv6 on ESP32
 *
 * Initializes VFS, loads filesystem, modules, and starts shell.
 */
void xv6_boot(void);

#endif
