#include "wifimod_state.h"
#include "wifimod_events.h"
#include "wifimod_wifi.h"

#include "esp_err.h"

static int g_driver_ready = 0;

esp_err_t wifi_init_driver(void)
{
  if(g_driver_ready)
    return ESP_ERR_INVALID_STATE;

  g_driver_ready = 1;
  wifi_set_state(WIFI_STATE_DOWN);
  return ESP_OK;
}

esp_err_t wifi_config_and_connect(const char *ssid, const char *password)
{
  if(!ssid || ssid[0] == 0)
    return ESP_ERR_INVALID_ARG;
  if(!g_driver_ready)
    return ESP_ERR_INVALID_STATE;

  wifi_set_config(ssid, password);
  wifi_set_state(WIFI_STATE_FAILED);
  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_disconnect(void)
{
  if(!g_driver_ready)
    return ESP_ERR_INVALID_STATE;

  wifi_set_state(WIFI_STATE_DOWN);
  wifi_reset_reconnect();
  return ESP_OK;
}

esp_err_t wifi_start_scan(const char *ssid, bool blocking)
{
  (void)ssid;
  (void)blocking;

  if(!g_driver_ready)
    return ESP_ERR_INVALID_STATE;

  wifi_set_state(WIFI_STATE_FAILED);
  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_get_scan_results(ap_info_t *ap_list, int max_count, int *found)
{
  (void)ap_list;
  (void)max_count;

  if(!ap_list || !found)
    return ESP_ERR_INVALID_ARG;

  *found = 0;
  return ESP_OK;
}

esp_err_t wifi_get_sta_rssi(int *rssi)
{
  if(!rssi)
    return ESP_ERR_INVALID_ARG;
  if(!g_driver_ready)
    return ESP_ERR_INVALID_STATE;

  *rssi = 0;
  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_deinit_driver(void)
{
  g_driver_ready = 0;
  wifi_set_state(WIFI_STATE_DOWN);
  return ESP_OK;
}
