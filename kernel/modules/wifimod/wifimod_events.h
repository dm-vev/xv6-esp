#ifndef WIFIMOD_EVENTS_H
#define WIFIMOD_EVENTS_H

#include "esp_err.h"

void handle_sta_disconnected(void);

esp_err_t wifi_events_register(void);
esp_err_t wifi_events_unregister(void);

#endif
