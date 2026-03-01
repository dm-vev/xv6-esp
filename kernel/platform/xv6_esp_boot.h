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

/**
 * @brief Initialize shared ESP-IDF network runtime primitives.
 *
 * Idempotent. Safe to call multiple times.
 *
 * @return 0 on success, -1 on failure.
 */
int xv6_network_runtime_init(void);

#endif
