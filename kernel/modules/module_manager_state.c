#include "modules/module_manager_internal.h"

/*
 * Central state for module tracking and lock-protected metadata updates.
 * Keeping these primitives in one unit simplifies reasoning about ownership.
 */
kmod_slot_t g_slots[KMOD_MAX_TRACKED];
kmod_config_t g_cfg = {
  XV6_KMOD_DEV_MODE_DEFAULT,
  XV6_KMOD_ALLOW_UNSIGNED_DEV_DEFAULT,
  XV6_KMOD_ALLOW_FORCE_UNLOAD_DEV_DEFAULT,
};
int g_next_module_id = 1;
char g_last_error[KMOD_ERR_MAX];
SemaphoreHandle_t g_kmod_mu;

void kmod_lock(void)
{
  if(g_kmod_mu == 0)
    g_kmod_mu = xSemaphoreCreateRecursiveMutex();
  if(g_kmod_mu)
    (void)xSemaphoreTakeRecursive(g_kmod_mu, portMAX_DELAY);
}

void kmod_unlock(void)
{
  if(g_kmod_mu)
    (void)xSemaphoreGiveRecursive(g_kmod_mu);
}

void set_last_error(const char *fmt, ...)
{
  va_list ap;

  if(fmt == 0)
    fmt = "kmod: unknown";

  va_start(ap, fmt);
  vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
  va_end(ap);
}

void copy_cstr(char *dst, int dst_len, const char *src)
{
  if(dst == 0 || dst_len <= 0)
    return;
  if(src == 0)
    src = "";
  strncpy(dst, src, (size_t)dst_len - 1u);
  dst[dst_len - 1] = 0;
}

int slot_index_by_id_locked(int module_id)
{
  int i;

  for(i = 0; i < KMOD_MAX_TRACKED; i++){
    if(g_slots[i].used && g_slots[i].module_id == module_id)
      return i;
  }
  return -1;
}

int slot_index_by_path_locked(const char *path)
{
  int i;

  for(i = 0; i < KMOD_MAX_TRACKED; i++){
    if(g_slots[i].used && strcmp(g_slots[i].path, path) == 0)
      return i;
  }
  return -1;
}

int slot_index_by_name_locked(const char *name)
{
  int i;

  if(name == 0 || name[0] == 0)
    return -1;

  for(i = 0; i < KMOD_MAX_TRACKED; i++){
    if(g_slots[i].used && strcmp(g_slots[i].name, name) == 0)
      return i;
  }
  return -1;
}

int alloc_slot_locked(void)
{
  int i;

  for(i = 0; i < KMOD_MAX_TRACKED; i++){
    if(!g_slots[i].used)
      return i;
  }
  return -1;
}

int alloc_module_id_locked(void)
{
  int attempts;

  /*
   * Avoid ID reuse collisions after wrap-around.
   * KMOD_MAX_TRACKED is small, but monotonic IDs are user-visible.
   */
  for(attempts = 0; attempts < 0x7fffffff; attempts++){
    int candidate = g_next_module_id++;
    if(g_next_module_id <= 0)
      g_next_module_id = 1;
    if(candidate <= 0)
      continue;
    if(slot_index_by_id_locked(candidate) < 0)
      return candidate;
  }

  return -1;
}

const char *kmod_last_error(void)
{
  if(g_last_error[0] == 0)
    return "ok";
  return g_last_error;
}

int kmod_dev_mode(void)
{
  return g_cfg.dev_mode;
}
