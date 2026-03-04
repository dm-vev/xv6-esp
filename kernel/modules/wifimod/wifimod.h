#ifndef WIFIMOD_H
#define WIFIMOD_H

#include <stdint.h>
#include <stdbool.h>

#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASS_MAX_LEN 64
#define MAX_AP_COUNT 16

typedef enum {
  WIFI_STATE_DOWN = 0,
  WIFI_STATE_STARTING,
  WIFI_STATE_SCANNING,
  WIFI_STATE_CONNECTING,
  WIFI_STATE_CONNECTED,
  WIFI_STATE_DISCONNECTING,
  WIFI_STATE_FAILED,
} wifi_state_t;

typedef struct {
  char ssid[33];
  int8_t rssi;
  uint8_t auth;
} ap_info_t;

int wifi_kmod_connect(const char *ssid, const char *password);
int wifi_kmod_disconnect(void);
int wifi_kmod_scan(const char *ssid, ap_info_t *ap_list, int max_count, int *found);
int wifi_kmod_status(void);
int wifi_kmod_get_rssi(int *rssi);
int wifi_kmod_set_trace(int enabled);

#endif
