#include "wifimod_state.h"

#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"

#define WIFI_RECONNECT_MAX_ATTEMPTS 5

static void handle_wifi_event(int32_t event_id, void *event_data)
{
  (void)event_data;
  
  switch(event_id){
  case WIFI_EVENT_WIFI_READY:
    wifi_trace("wifi ready");
    break;
  case WIFI_EVENT_WIFI_STOP:
    wifi_trace("wifi stopped");
    wifi_set_state(WIFI_STATE_DOWN);
    break;
  case WIFI_EVENT_STA_START:
    wifi_trace("station started");
    break;
  case WIFI_EVENT_STA_CONNECTED:
    wifi_trace("connected to AP");
    wifi_set_state(WIFI_STATE_CONNECTED);
    wifi_reset_reconnect();
    break;
  case WIFI_EVENT_STA_DISCONNECTED:
    wifi_trace("disconnected from AP");
    handle_sta_disconnected();
    break;
  case WIFI_EVENT_SCAN_DONE:
    wifi_trace("scan done");
    wifi_set_state(WIFI_STATE_CONNECTED);
    break;
  default:
    break;
  }
}

static void handle_ip_event(int32_t event_id, void *event_data)
{
  switch(event_id){
  case IP_EVENT_STA_GOT_IP:
  {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    wifi_trace("got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    break;
  }
  case IP_EVENT_STA_LOST_IP:
    wifi_trace("lost IP");
    break;
  default:
    break;
  }
}

void handle_sta_disconnected(void)
{
  wifi_state_t state = wifi_get_state();
  
  if(state != WIFI_STATE_CONNECTED && state != WIFI_STATE_CONNECTING)
    return;
    
  uint8_t attempts = wifi_get_reconnect_attempts();
  if(attempts < WIFI_RECONNECT_MAX_ATTEMPTS){
    wifi_trace("reconnecting... attempt %d", attempts + 1);
    wifi_set_state(WIFI_STATE_CONNECTING);
    esp_wifi_connect();
    wifi_inc_reconnect();
  } else {
    wifi_trace("max reconnect attempts reached");
    wifi_set_state(WIFI_STATE_FAILED);
  }
}

esp_err_t wifi_events_register(void)
{
  esp_err_t err;
  
  err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, 
                                   (esp_event_handler_t)handle_wifi_event, NULL);
  if(err != ESP_OK)
    return err;
    
  err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                   (esp_event_handler_t)handle_ip_event, NULL);
  return err;
}

esp_err_t wifi_events_unregister(void)
{
  esp_err_t err;
  
  err = esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID);
  if(err != ESP_OK)
    return err;
    
  err = esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID);
  return err;
}
