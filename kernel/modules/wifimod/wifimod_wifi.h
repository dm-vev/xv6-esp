#ifndef WIFIMOD_WIFI_H
#define WIFIMOD_WIFI_H

#include <stdbool.h>

#include "esp_err.h"
#include "wifimod.h"

esp_err_t wifi_init_driver(void);
esp_err_t wifi_config_and_connect(const char *ssid, const char *password);
esp_err_t wifi_disconnect(void);
esp_err_t wifi_start_scan(const char *ssid, bool blocking);
esp_err_t wifi_get_scan_results(ap_info_t *ap_list, int max_count, int *found);
esp_err_t wifi_get_sta_rssi(int *rssi);
esp_err_t wifi_deinit_driver(void);

#endif
