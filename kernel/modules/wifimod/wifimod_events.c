#include "wifimod_state.h"
#include "wifimod_events.h"

#include "esp_err.h"

void handle_sta_disconnected(void)
{
  wifi_set_state(WIFI_STATE_FAILED);
}

esp_err_t wifi_events_register(void)
{
  return ESP_OK;
}

esp_err_t wifi_events_unregister(void)
{
  return ESP_OK;
}
