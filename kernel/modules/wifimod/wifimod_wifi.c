#include "wifimod_state.h"
#include "wifimod_events.h"

#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"

#include <string.h>

esp_err_t wifi_init_driver(void)
{
  esp_err_t err;
  esp_netif_t *netif;

  err = esp_event_loop_create_default();
  if(err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    return err;

  netif = esp_netif_create_default_wifi_sta();
  if(netif == NULL)
    return ESP_FAIL;

  wifi_set_netif(netif);

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&cfg);
  if(err != ESP_OK)
    return err;

  err = esp_wifi_set_mode(WIFI_MODE_STA);
  if(err != ESP_OK)
    return err;

  err = esp_wifi_start();
  return err;
}

esp_err_t wifi_config_and_connect(const char *ssid, const char *password)
{
  esp_err_t err;
  wifi_config_t sta_cfg;
  size_t ssid_len = strlen(ssid);

  memset(&sta_cfg, 0, sizeof(sta_cfg));
  memcpy(sta_cfg.sta.ssid, ssid, ssid_len > 32 ? 32 : ssid_len);
  sta_cfg.sta.ssid[32] = 0;

  if(password && strlen(password) > 0){
    size_t pass_len = strlen(password);
    memcpy(sta_cfg.sta.password, password, pass_len > 64 ? 64 : pass_len);
    sta_cfg.sta.password[64] = 0;
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  } else {
    sta_cfg.sta.password[0] = 0;
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
  }

  sta_cfg.sta.pmf_cfg.capable = 1;
  sta_cfg.sta.pmf_cfg.required = 0;
  sta_cfg.sta.channel = 0;
  sta_cfg.sta.bssid_set = 0;

  err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
  if(err != ESP_OK)
    return err;

  wifi_set_config(ssid, password);
  wifi_set_state(WIFI_STATE_CONNECTING);
  wifi_reset_reconnect();

  err = esp_wifi_connect();
  return err;
}

esp_err_t wifi_disconnect(void)
{
  esp_err_t err;
  
  wifi_set_state(WIFI_STATE_DISCONNECTING);
  err = esp_wifi_disconnect();
  
  if(err == ESP_OK)
    wifi_set_state(WIFI_STATE_DOWN);
    
  return err;
}

esp_err_t wifi_start_scan(const char *ssid, bool blocking)
{
  wifi_scan_config_t scan_cfg = {
    .ssid = (uint8_t *)((ssid && strlen(ssid) > 0) ? ssid : NULL),
    .bssid = NULL,
    .channel = 0,
    .show_hidden = 0,
    .scan_type = WIFI_SCAN_TYPE_ACTIVE,
  };

  wifi_set_state(WIFI_STATE_SCANNING);
  return esp_wifi_scan_start(&scan_cfg, blocking);
}

esp_err_t wifi_get_scan_results(ap_info_t *ap_list, int max_count, int *found)
{
  esp_err_t err;
  uint16_t number = (uint16_t)(max_count > 0 ? max_count : MAX_AP_COUNT);
  wifi_ap_record_t *ap_records;
  
  if(!ap_list || !found)
    return ESP_ERR_INVALID_ARG;
  
  ap_records = (wifi_ap_record_t *)malloc(number * sizeof(wifi_ap_record_t));
  if(!ap_records)
    return ESP_ERR_NO_MEM;
  
  err = esp_wifi_scan_get_ap_records(&number, ap_records);
  if(err != ESP_OK){
    free(ap_records);
    return err;
  }
  
  int count = (number > (uint16_t)max_count) ? max_count : (int)number;
  
  for(int i = 0; i < count; i++){
    memset(ap_list[i].ssid, 0, sizeof(ap_list[i].ssid));
    strncpy(ap_list[i].ssid, (const char *)ap_records[i].ssid, sizeof(ap_list[i].ssid) - 1);
    ap_list[i].rssi = ap_records[i].rssi;
    ap_list[i].auth = ap_records[i].authmode;
  }
  
  *found = count;
  free(ap_records);
  
  return ESP_OK;
}

esp_err_t wifi_get_sta_rssi(int *rssi)
{
  wifi_ap_record_t ap_info;
  esp_err_t err;
  
  if(!rssi)
    return ESP_ERR_INVALID_ARG;
    
  err = esp_wifi_sta_get_rssi(&ap_info);
  if(err == ESP_OK)
    *rssi = (int)ap_info.rssi;
    
  return err;
}

esp_err_t wifi_deinit_driver(void)
{
  esp_err_t err;
  
  err = esp_wifi_stop();
  if(err != ESP_OK)
    return err;
    
  err = esp_wifi_deinit();
  if(err != ESP_OK)
    return err;
    
  return ESP_OK;
}
