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
char g_load_stack[KMOD_MAX_TRACKED][MAXPATH];
int g_load_stack_depth;

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

static uint32 slot_bit_for_idx(int idx)
{
  if(idx < 0 || idx >= 32)
    return 0;
  return (uint32)1u << (uint32)idx;
}

int dependency_refcnt_locked(int module_idx)
{
  uint32 bit;
  int i;
  int count = 0;

  bit = slot_bit_for_idx(module_idx);
  if(bit == 0)
    return 0;

  for(i = 0; i < KMOD_MAX_TRACKED; i++){
    if(!g_slots[i].used)
      continue;
    if((g_slots[i].deps_mask & bit) != 0)
      count++;
  }
  return count;
}

int total_refcnt_locked(int module_idx)
{
  if(module_idx < 0 || module_idx >= KMOD_MAX_TRACKED || !g_slots[module_idx].used)
    return 0;
  return g_slots[module_idx].ext_refcnt + dependency_refcnt_locked(module_idx);
}

int module_reaches_locked(int from_idx, int target_idx)
{
  uint32 target_bit;
  uint32 frontier;
  uint32 visited = 0;

  if(from_idx < 0 || from_idx >= KMOD_MAX_TRACKED || target_idx < 0 || target_idx >= KMOD_MAX_TRACKED)
    return 0;
  if(from_idx == target_idx)
    return 1;
  if(!g_slots[from_idx].used || !g_slots[target_idx].used)
    return 0;

  frontier = slot_bit_for_idx(from_idx);
  target_bit = slot_bit_for_idx(target_idx);
  while(frontier != 0){
    uint32 next = 0;
    int i;

    if((frontier & target_bit) != 0)
      return 1;

    visited |= frontier;
    for(i = 0; i < KMOD_MAX_TRACKED; i++){
      uint32 bit = slot_bit_for_idx(i);
      if((frontier & bit) == 0)
        continue;
      if(g_slots[i].used)
        next |= g_slots[i].deps_mask;
    }

    frontier = next & ~visited;
  }
  return 0;
}

int add_dependency_edge_locked(int consumer_idx, int provider_idx)
{
  uint32 provider_bit;

  if(consumer_idx < 0 || consumer_idx >= KMOD_MAX_TRACKED || provider_idx < 0 || provider_idx >= KMOD_MAX_TRACKED)
    return -1;
  if(!g_slots[consumer_idx].used || !g_slots[provider_idx].used)
    return -1;
  if(consumer_idx == provider_idx)
    return -1;
  if(module_reaches_locked(provider_idx, consumer_idx))
    return -1;

  provider_bit = slot_bit_for_idx(provider_idx);
  if((g_slots[consumer_idx].deps_mask & provider_bit) != 0)
    return 0;

  g_slots[consumer_idx].deps_mask |= provider_bit;
  return 0;
}

void remove_dependencies_from_module_locked(int consumer_idx)
{
  if(consumer_idx < 0 || consumer_idx >= KMOD_MAX_TRACKED || !g_slots[consumer_idx].used)
    return;
  g_slots[consumer_idx].deps_mask = 0;
}

void remove_all_edges_to_module_locked(int provider_idx)
{
  uint32 provider_bit;
  int i;

  provider_bit = slot_bit_for_idx(provider_idx);
  if(provider_bit == 0)
    return;

  for(i = 0; i < KMOD_MAX_TRACKED; i++){
    if(!g_slots[i].used || i == provider_idx)
      continue;
    g_slots[i].deps_mask &= ~provider_bit;
  }
}

int load_stack_contains_locked(const char *path)
{
  int i;

  if(path == 0 || path[0] == 0)
    return 0;

  for(i = 0; i < g_load_stack_depth; i++){
    if(strcmp(g_load_stack[i], path) == 0)
      return 1;
  }
  return 0;
}

int load_stack_push_locked(const char *path)
{
  if(path == 0 || path[0] == 0)
    return -1;
  if(g_load_stack_depth >= KMOD_MAX_TRACKED)
    return -1;
  copy_cstr(g_load_stack[g_load_stack_depth], (int)sizeof(g_load_stack[g_load_stack_depth]), path);
  g_load_stack_depth++;
  return 0;
}

void load_stack_pop_locked(void)
{
  if(g_load_stack_depth <= 0)
    return;
  g_load_stack_depth--;
  g_load_stack[g_load_stack_depth][0] = 0;
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
