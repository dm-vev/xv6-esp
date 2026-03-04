#include "wifimod.h"
#include "wifimod_state.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static volatile int g_lock;
static wifi_state_t g_state = WIFI_STATE_DOWN;
static char g_ssid[WIFI_SSID_MAX_LEN + 1];
static char g_password[WIFI_PASS_MAX_LEN + 1];
static uint8_t g_reconnect_attempts = 0;
static int g_trace_verbose = 1;
static esp_netif_t *g_netif = NULL;

void wifi_lock(void)
{
  while(__sync_lock_test_and_set(&g_lock, 1))
    ;
}

void wifi_unlock(void)
{
  __sync_lock_release(&g_lock);
}

wifi_state_t wifi_get_state(void)
{
  return g_state;
}

void wifi_set_state(wifi_state_t state)
{
  g_state = state;
}

void wifi_inc_reconnect(void)
{
  g_reconnect_attempts++;
}

void wifi_reset_reconnect(void)
{
  g_reconnect_attempts = 0;
}

uint8_t wifi_get_reconnect_attempts(void)
{
  return g_reconnect_attempts;
}

void wifi_get_config(char *ssid, char *password)
{
  if(ssid){
    strncpy(ssid, g_ssid, WIFI_SSID_MAX_LEN);
    ssid[WIFI_SSID_MAX_LEN] = 0;
  }
  if(password){
    strncpy(password, g_password, WIFI_PASS_MAX_LEN);
    password[WIFI_PASS_MAX_LEN] = 0;
  }
}

void wifi_set_config(const char *ssid, const char *password)
{
  if(ssid && ssid[0] != 0){
    strncpy(g_ssid, ssid, WIFI_SSID_MAX_LEN);
    g_ssid[WIFI_SSID_MAX_LEN] = 0;
  } else {
    g_ssid[0] = 0;
  }
  if(password && password[0] != 0){
    strncpy(g_password, password, WIFI_PASS_MAX_LEN);
    g_password[WIFI_PASS_MAX_LEN] = 0;
  } else {
    g_password[0] = 0;
  }
}

esp_netif_t *wifi_get_netif(void)
{
  return g_netif;
}

void wifi_set_netif(esp_netif_t *netif)
{
  g_netif = netif;
}

int wifi_is_trace_enabled(void)
{
  return g_trace_verbose;
}

void wifi_set_trace_enabled(int enabled)
{
  g_trace_verbose = enabled ? 1 : 0;
}

void wifi_trace(const char *fmt, ...)
{
  if(!g_trace_verbose)
    return;
  va_list ap;
  va_start(ap, fmt);
  printf("[wifi_kmod] ");
  vprintf(fmt, ap);
  printf("\n");
  va_end(ap);
}
