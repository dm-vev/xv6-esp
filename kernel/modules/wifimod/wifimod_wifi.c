#include "wifimod_state.h"
#include "wifimod_wifi.h"

#include <string.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"

static int g_driver_ready = 0;

extern esp_err_t xv6_wifi_host_init_default(void);

static void wifi_update_link_state(void)
{
  wifi_ap_record_t ap;

  if(!g_driver_ready){
    wifi_set_state(WIFI_STATE_DOWN);
    return;
  }

  if(esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    wifi_set_state(WIFI_STATE_CONNECTED);
  else
    wifi_set_state(WIFI_STATE_DOWN);
}

esp_err_t wifi_init_driver(void)
{
  esp_err_t err;
  esp_err_t cleanup_err;
  esp_netif_t *netif;
  int created_netif = 0;

  if(g_driver_ready)
    return ESP_ERR_INVALID_STATE;

  wifi_set_state(WIFI_STATE_STARTING);

  netif = wifi_get_netif();
  if(!netif){
    netif = esp_netif_create_default_wifi_sta();
    if(!netif){
      wifi_set_state(WIFI_STATE_FAILED);
      return ESP_FAIL;
    }
    wifi_set_netif(netif);
    created_netif = 1;
  }

  err = xv6_wifi_host_init_default();
  if(err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    wifi_trace("esp_wifi_init returned err=%d", err);

  err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if(err != ESP_OK)
    goto init_fail;

  err = esp_wifi_set_mode(WIFI_MODE_STA);
  if(err != ESP_OK)
    goto init_fail;

  err = esp_wifi_start();
  if(err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    goto init_fail;

  g_driver_ready = 1;
  wifi_set_state(WIFI_STATE_DOWN);
  return ESP_OK;

init_fail:
  cleanup_err = esp_wifi_stop();
  (void)cleanup_err;
  cleanup_err = esp_wifi_deinit();
  (void)cleanup_err;
  if(created_netif){
    esp_netif_destroy(netif);
    wifi_set_netif(NULL);
  }
  wifi_set_state(WIFI_STATE_FAILED);
  return err;
}

esp_err_t wifi_config_and_connect(const char *ssid, const char *password)
{
  wifi_config_t cfg;
  size_t ssid_len;
  size_t pass_len = 0;
  esp_err_t err;

  if(!ssid || ssid[0] == 0)
    return ESP_ERR_INVALID_ARG;
  if(!g_driver_ready)
    return ESP_ERR_INVALID_STATE;

  ssid_len = strlen(ssid);
  if(ssid_len > WIFI_SSID_MAX_LEN)
    return ESP_ERR_INVALID_ARG;

  if(password && password[0] != 0){
    pass_len = strlen(password);
    if(pass_len > WIFI_PASS_MAX_LEN)
      return ESP_ERR_INVALID_ARG;
  }

  memset(&cfg, 0, sizeof(cfg));
  memcpy(cfg.sta.ssid, ssid, ssid_len);
  if(password && pass_len > 0)
    memcpy(cfg.sta.password, password, pass_len);

  err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
  if(err != ESP_OK){
    wifi_set_state(WIFI_STATE_FAILED);
    return err;
  }

  wifi_set_config(ssid, password);
  wifi_reset_reconnect();
  wifi_set_state(WIFI_STATE_CONNECTING);

  err = esp_wifi_connect();
  if(err != ESP_OK){
    wifi_set_state(WIFI_STATE_FAILED);
    return err;
  }

  return ESP_OK;
}

esp_err_t wifi_disconnect(void)
{
  esp_err_t err;

  if(!g_driver_ready)
    return ESP_ERR_INVALID_STATE;

  wifi_set_state(WIFI_STATE_DISCONNECTING);
  err = esp_wifi_disconnect();
  if(err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    wifi_trace("esp_wifi_disconnect returned err=%d", err);

  wifi_set_state(WIFI_STATE_DOWN);
  wifi_reset_reconnect();
  return ESP_OK;
}

esp_err_t wifi_start_scan(const char *ssid, bool blocking)
{
  wifi_scan_config_t cfg;
  esp_err_t err;
  size_t ssid_len = 0;

  if(!g_driver_ready)
    return ESP_ERR_INVALID_STATE;

  if(ssid && ssid[0] != 0){
    ssid_len = strlen(ssid);
    if(ssid_len > WIFI_SSID_MAX_LEN)
      return ESP_ERR_INVALID_ARG;
  }

  memset(&cfg, 0, sizeof(cfg));
  if(ssid && ssid[0] != 0)
    cfg.ssid = (uint8_t *)ssid;

  wifi_set_state(WIFI_STATE_SCANNING);
  err = esp_wifi_scan_start(&cfg, blocking);
  if(err != ESP_OK){
    wifi_set_state(WIFI_STATE_FAILED);
    return err;
  }

  if(blocking)
    wifi_update_link_state();

  return ESP_OK;
}

esp_err_t wifi_get_scan_results(ap_info_t *ap_list, int max_count, int *found)
{
  esp_err_t err;
  uint16_t ap_num = 0;
  uint16_t out_count;
  int i;
  wifi_ap_record_t ap_records[MAX_AP_COUNT];

  if(!ap_list || !found || max_count <= 0)
    return ESP_ERR_INVALID_ARG;
  if(!g_driver_ready)
    return ESP_ERR_INVALID_STATE;

  if(max_count > MAX_AP_COUNT)
    max_count = MAX_AP_COUNT;

  err = esp_wifi_scan_get_ap_num(&ap_num);
  if(err != ESP_OK)
    return err;

  if(ap_num == 0){
    *found = 0;
    return ESP_OK;
  }

  out_count = (uint16_t)max_count;
  if(ap_num < out_count)
    out_count = ap_num;

  err = esp_wifi_scan_get_ap_records(&out_count, ap_records);
  if(err != ESP_OK)
    return err;

  for(i = 0; i < (int)out_count; i++){
    memset(&ap_list[i], 0, sizeof(ap_list[i]));
    strncpy(ap_list[i].ssid, (const char *)ap_records[i].ssid, WIFI_SSID_MAX_LEN);
    ap_list[i].ssid[WIFI_SSID_MAX_LEN] = 0;
    ap_list[i].rssi = ap_records[i].rssi;
    ap_list[i].auth = (uint8_t)ap_records[i].authmode;
  }

  *found = (int)out_count;
  return ESP_OK;
}

esp_err_t wifi_get_sta_rssi(int *rssi)
{
  wifi_ap_record_t ap;
  esp_err_t err;

  if(!rssi)
    return ESP_ERR_INVALID_ARG;
  if(!g_driver_ready)
    return ESP_ERR_INVALID_STATE;

  err = esp_wifi_sta_get_ap_info(&ap);
  if(err != ESP_OK){
    wifi_update_link_state();
    return err;
  }

  *rssi = (int)ap.rssi;
  wifi_set_state(WIFI_STATE_CONNECTED);
  return ESP_OK;
}

esp_err_t wifi_deinit_driver(void)
{
  esp_err_t err;
  esp_err_t first_err = ESP_OK;
  esp_netif_t *netif = wifi_get_netif();

  if(g_driver_ready){
    err = esp_wifi_stop();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE && first_err == ESP_OK)
      first_err = err;

    err = esp_wifi_deinit();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE && first_err == ESP_OK)
      first_err = err;
  }

  if(netif){
    esp_netif_destroy(netif);
    wifi_set_netif(NULL);
  }

  g_driver_ready = 0;
  wifi_set_config(NULL, NULL);
  wifi_reset_reconnect();
  wifi_set_state(WIFI_STATE_DOWN);
  return first_err;
}
