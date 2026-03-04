#include "wifimod_state.h"
#include "wifimod_events.h"

#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#define WIFI_RECONNECT_MAX_ATTEMPTS 5

static int g_events_registered;
static esp_event_handler_instance_t g_wifi_handler;
static esp_event_handler_instance_t g_ip_handler;

static void wifi_update_link_state(void)
{
  wifi_ap_record_t ap;

  if(esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    wifi_set_state(WIFI_STATE_CONNECTED);
  else
    wifi_set_state(WIFI_STATE_DOWN);
}

void handle_sta_disconnected(void)
{
  esp_err_t err;
  uint8_t attempts;
  wifi_state_t state = wifi_get_state();

  if(state == WIFI_STATE_DISCONNECTING || state == WIFI_STATE_DOWN){
    wifi_set_state(WIFI_STATE_DOWN);
    wifi_reset_reconnect();
    return;
  }
  if(state == WIFI_STATE_SCANNING){
    wifi_set_state(WIFI_STATE_DOWN);
    return;
  }

  attempts = wifi_get_reconnect_attempts();
  if(attempts >= WIFI_RECONNECT_MAX_ATTEMPTS){
    wifi_set_state(WIFI_STATE_FAILED);
    wifi_trace("reconnect exhausted (%u attempts)", (unsigned)attempts);
    return;
  }

  wifi_inc_reconnect();
  attempts = wifi_get_reconnect_attempts();
  wifi_set_state(WIFI_STATE_CONNECTING);

  err = esp_wifi_connect();
  if(err != ESP_OK){
    wifi_set_state(WIFI_STATE_FAILED);
    wifi_trace("reconnect %u/%u failed err=%d", (unsigned)attempts, (unsigned)WIFI_RECONNECT_MAX_ATTEMPTS, err);
    return;
  }

  wifi_trace("reconnect %u/%u", (unsigned)attempts, (unsigned)WIFI_RECONNECT_MAX_ATTEMPTS);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
  (void)arg;
  (void)event_base;
  (void)event_data;

  switch(event_id){
    case WIFI_EVENT_STA_START:
      if(wifi_get_state() == WIFI_STATE_STARTING)
        wifi_set_state(WIFI_STATE_DOWN);
      break;
    case WIFI_EVENT_STA_CONNECTED:
      wifi_trace("station connected");
      break;
    case WIFI_EVENT_STA_DISCONNECTED:
      wifi_trace("station disconnected");
      handle_sta_disconnected();
      break;
    case WIFI_EVENT_SCAN_DONE:
      if(wifi_get_state() == WIFI_STATE_SCANNING)
        wifi_update_link_state();
      break;
    default:
      break;
  }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
  (void)arg;
  (void)event_base;
  (void)event_data;

  if(event_id == IP_EVENT_STA_GOT_IP){
    wifi_reset_reconnect();
    wifi_set_state(WIFI_STATE_CONNECTED);
    wifi_trace("got station ip");
  }
}

esp_err_t wifi_events_register(void)
{
  esp_err_t err;

  if(g_events_registered)
    return ESP_OK;

  err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &g_wifi_handler);
  if(err != ESP_OK)
    return err;

  err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL, &g_ip_handler);
  if(err != ESP_OK){
    (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, g_wifi_handler);
    g_wifi_handler = NULL;
    return err;
  }

  g_events_registered = 1;
  return ESP_OK;
}

esp_err_t wifi_events_unregister(void)
{
  esp_err_t err = ESP_OK;
  esp_err_t first_err = ESP_OK;

  if(!g_events_registered)
    return ESP_OK;

  if(g_wifi_handler){
    err = esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, g_wifi_handler);
    if(err != ESP_OK && first_err == ESP_OK)
      first_err = err;
    g_wifi_handler = NULL;
  }

  if(g_ip_handler){
    err = esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, g_ip_handler);
    if(err != ESP_OK && first_err == ESP_OK)
      first_err = err;
    g_ip_handler = NULL;
  }

  g_events_registered = 0;
  if(first_err != ESP_OK)
    return first_err;
  return ESP_OK;
}
