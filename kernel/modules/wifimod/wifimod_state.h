#ifndef WIFIMOD_STATE_H
#define WIFIMOD_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#include "wifimod.h"

typedef struct esp_netif_obj esp_netif_t;

void wifi_lock(void);
void wifi_unlock(void);

wifi_state_t wifi_get_state(void);
void wifi_set_state(wifi_state_t state);

void wifi_inc_reconnect(void);
void wifi_reset_reconnect(void);
uint8_t wifi_get_reconnect_attempts(void);

void wifi_get_config(char *ssid, char *password);
void wifi_set_config(const char *ssid, const char *password);

esp_netif_t *wifi_get_netif(void);
void wifi_set_netif(esp_netif_t *netif);

int wifi_is_trace_enabled(void);
void wifi_set_trace_enabled(int enabled);
void wifi_trace(const char *fmt, ...);

#endif
