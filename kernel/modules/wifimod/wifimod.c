#include "wifimod.h"
#include "wifimod_state.h"
#include "wifimod_wifi.h"

#include "xv6_module.h"

static int g_trace_verbose = 1;

int wifi_kmod_connect(const char *ssid, const char *password)
{
  esp_err_t err;
  
  if(!ssid || strlen(ssid) == 0 || strlen(ssid) > WIFI_SSID_MAX_LEN){
    errno = EINVAL;
    return -1;
  }
  
  wifi_state_t state = wifi_get_state();
  if(state == WIFI_STATE_DOWN || state == WIFI_STATE_FAILED){
    err = wifi_init_driver();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE){
      wifi_trace("init driver failed: %d", err);
      errno = EIO;
      return -1;
    }
    
    err = wifi_events_register();
    if(err != ESP_OK){
      wifi_trace("register events failed: %d", err);
      errno = EIO;
      return -1;
    }
  }
  
  wifi_lock();
  err = wifi_config_and_connect(ssid, password);
  wifi_unlock();
  
  if(err != ESP_OK){
    wifi_trace("connect failed: %d", err);
    errno = EIO;
    return -1;
  }
  
  wifi_trace("connecting to %s...", ssid);
  return 0;
}

int wifi_kmod_disconnect(void)
{
  esp_err_t err;
  
  wifi_lock();
  err = wifi_disconnect();
  wifi_unlock();
  
  if(err != ESP_OK && err != ESP_ERR_INVALID_STATE){
    errno = EIO;
    return -1;
  }
  
  wifi_events_unregister();
  wifi_deinit_driver();
  
  wifi_trace("disconnected");
  return 0;
}

int wifi_kmod_scan(const char *ssid, ap_info_t *ap_list, int max_count, int *found)
{
  esp_err_t err;
  
  if(!ap_list || !found){
    errno = EINVAL;
    return -1;
  }
  
  wifi_state_t state = wifi_get_state();
  if(state == WIFI_STATE_DOWN || state == WIFI_STATE_FAILED){
    err = wifi_init_driver();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE){
      wifi_trace("init driver failed: %d", err);
      *found = 0;
      return -1;
    }
    
    err = wifi_events_register();
    if(err != ESP_OK){
      wifi_trace("register events failed: %d", err);
      *found = 0;
      return -1;
    }
  }
  
  wifi_lock();
  err = wifi_start_scan(ssid, true);
  if(err != ESP_OK){
    wifi_trace("scan start failed: %d", err);
    wifi_unlock();
    *found = 0;
    return -1;
  }
  
  err = wifi_get_scan_results(ap_list, max_count, found);
  wifi_unlock();
  
  if(err != ESP_OK){
    wifi_trace("scan get results failed: %d", err);
    *found = 0;
    return -1;
  }
  
  wifi_trace("scan found %d APs", *found);
  return 0;
}

int wifi_kmod_status(void)
{
  return (int)wifi_get_state();
}

int wifi_kmod_get_rssi(int *rssi)
{
  esp_err_t err;
  
  if(!rssi){
    errno = EINVAL;
    return -1;
  }
  
  err = wifi_get_sta_rssi(rssi);
  if(err != ESP_OK){
    *rssi = 0;
    return -1;
  }
  
  return 0;
}

int wifi_kmod_set_trace(int enabled)
{
  g_trace_verbose = enabled ? 1 : 0;
  return 0;
}

int xv6_module_init(void)
{
  wifi_lock();
  memset(&g_trace_verbose, 0, sizeof(g_trace_verbose));
  g_trace_verbose = 1;
  wifi_set_state(WIFI_STATE_DOWN);
  wifi_reset_reconnect();
  wifi_unlock();
  
  wifi_trace("init");
  return 0;
}

int xv6_module_fini(void)
{
  wifi_lock();
  wifi_events_unregister();
  wifi_deinit_driver();
  wifi_set_state(WIFI_STATE_DOWN);
  wifi_unlock();
  
  wifi_trace("fini");
  return 0;
}

static const xv6_module_symbol_t g_symbols[] = {
  { "wifi_kmod_connect", (void *)wifi_kmod_connect, XV6_MODULE_SYMBOL_EXTENSION, 50 },
  { "wifi_kmod_disconnect", (void *)wifi_kmod_disconnect, XV6_MODULE_SYMBOL_EXTENSION, 50 },
  { "wifi_kmod_scan", (void *)wifi_kmod_scan, XV6_MODULE_SYMBOL_EXTENSION, 50 },
  { "wifi_kmod_status", (void *)wifi_kmod_status, XV6_MODULE_SYMBOL_EXTENSION, 50 },
  { "wifi_kmod_get_rssi", (void *)wifi_kmod_get_rssi, XV6_MODULE_SYMBOL_EXTENSION, 50 },
  { "wifi_kmod_set_trace", (void *)wifi_kmod_set_trace, XV6_MODULE_SYMBOL_EXTENSION, 50 },
};

static const xv6_module_desc_t g_desc = {
  KMOD_MODULE_ABI_VER,
  "wifi_kmod",
  50,
  g_symbols,
  (int)(sizeof(g_symbols) / sizeof(g_symbols[0])),
};

const xv6_module_desc_t *xv6_module_describe(void)
{
  return &g_desc;
}
