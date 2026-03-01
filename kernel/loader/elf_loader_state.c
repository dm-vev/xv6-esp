#include "loader/elf_loader_internal.h"

/*
 * Loader global state and synchronization primitives are centralized here so
 * lock ownership is explicit and not scattered across relocation/link logic.
 */
const char *g_elf_loader_tag = "xv6_elf";

char g_dlerror_msg[128];
int g_dlerror_set = 0;

elf_module_t g_modules[ELFLOADER_MAX_MODULES];
int g_module_used[ELFLOADER_MAX_MODULES];
uint32 g_module_generation = 1;

elf_host_symbol_t g_host_syms[ELFLOADER_MAX_HOST_SYMBOLS];
int g_host_sym_count;

SemaphoreHandle_t g_module_mu;
elf_call_ctx_t g_call_ctx[ELF_CALL_CTX_MAX];
SemaphoreHandle_t g_call_ctx_mu;

_Static_assert(ELF_CALL_CTX_MAX >= 8, "ELF_CALL_CTX_MAX too small for concurrent applets");

void call_ctx_lock(void)
{
  if(g_call_ctx_mu == 0)
    g_call_ctx_mu = xSemaphoreCreateRecursiveMutex();
  if(g_call_ctx_mu)
    (void)xSemaphoreTakeRecursive(g_call_ctx_mu, portMAX_DELAY);
}

void call_ctx_unlock(void)
{
  if(g_call_ctx_mu)
    (void)xSemaphoreGiveRecursive(g_call_ctx_mu);
}

int call_ctx_set_current(elf_module_t *mod)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  int i;
  int free_i = -1;

  call_ctx_lock();
  for(i = 0; i < ELF_CALL_CTX_MAX; i++){
    if(g_call_ctx[i].task == self){
      if(mod == 0){
        g_call_ctx[i].task = 0;
        g_call_ctx[i].mod = 0;
        g_call_ctx[i].jb_valid = 0;
      } else {
        g_call_ctx[i].mod = mod;
      }
      call_ctx_unlock();
      return 0;
    }
    if(g_call_ctx[i].task == 0 && free_i < 0)
      free_i = i;
  }

  if(mod == 0){
    call_ctx_unlock();
    return 0;
  }

  if(free_i >= 0){
    g_call_ctx[free_i].task = self;
    g_call_ctx[free_i].mod = mod;
    g_call_ctx[free_i].jb_valid = 0;
    call_ctx_unlock();
    return 0;
  }

  call_ctx_unlock();
  return (mod == 0) ? 0 : -1;
}

elf_module_t *call_ctx_get_current(void)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  int i;
  elf_module_t *m = 0;

  call_ctx_lock();
  for(i = 0; i < ELF_CALL_CTX_MAX; i++){
    if(g_call_ctx[i].task == self){
      m = g_call_ctx[i].mod;
      break;
    }
  }
  call_ctx_unlock();
  return m;
}

elf_call_ctx_t *call_ctx_get_current_slot(void)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  int i;
  elf_call_ctx_t *slot = 0;

  call_ctx_lock();
  for(i = 0; i < ELF_CALL_CTX_MAX; i++){
    if(g_call_ctx[i].task == self){
      slot = &g_call_ctx[i];
      break;
    }
  }
  call_ctx_unlock();
  return slot;
}

int u32_range_valid(uint32 off, uint32 len, uint32 size)
{
  if(off > size)
    return 0;
  if(len > size - off)
    return 0;
  return 1;
}

int vaddr_offset_in_range(uint32 base, uint32 len, uint32 addr, uint32 *out_off)
{
  uint32 off;

  if(len == 0 || addr < base)
    return 0;
  off = addr - base;
  if(off >= len)
    return 0;
  if(out_off)
    *out_off = off;
  return 1;
}

int module_is_active_in_call_ctx(elf_module_t *mod)
{
  int i;
  int active = 0;

  if(mod == 0)
    return 0;

  call_ctx_lock();
  for(i = 0; i < ELF_CALL_CTX_MAX; i++){
    if(g_call_ctx[i].task != 0 && g_call_ctx[i].mod == mod){
      active = 1;
      break;
    }
  }
  call_ctx_unlock();
  return active;
}

void module_lock(void)
{
  if(g_module_mu == 0)
    g_module_mu = xSemaphoreCreateRecursiveMutex();
  if(g_module_mu)
    (void)xSemaphoreTakeRecursive(g_module_mu, portMAX_DELAY);
}

void module_unlock(void)
{
  if(g_module_mu)
    (void)xSemaphoreGiveRecursive(g_module_mu);
}

int module_index_from_ptr_locked(const void *ptr)
{
  int i;

  for(i = 0; i < ELFLOADER_MAX_MODULES; i++){
    if(g_module_used[i] && ptr == (const void *)&g_modules[i])
      return i;
  }
  return -1;
}

int module_is_live_locked(const elf_module_t *mod)
{
  return module_index_from_ptr_locked((const void *)mod) >= 0;
}

void elf_loader_task_cleanup_for_handle(void *task_handle)
{
  TaskHandle_t target = (TaskHandle_t)task_handle;
  elf_module_t *mod = 0;
  int mod_idx = -1;
  int i;

  if(target == 0)
    return;

  call_ctx_lock();
  for(i = 0; i < ELF_CALL_CTX_MAX; i++){
    if(g_call_ctx[i].task != target)
      continue;
    mod = g_call_ctx[i].mod;
    memset(&g_call_ctx[i], 0, sizeof(g_call_ctx[i]));
    break;
  }
  call_ctx_unlock();

  if(mod == 0)
    return;

  module_lock();
  mod_idx = module_index_from_ptr_locked((const void *)mod);
  if(mod_idx >= 0){
    if(mod->active_calls > 0)
      mod->active_calls--;
    if(mod->open_count <= 0 && mod->active_calls <= 0 && mod->dependent_count <= 0)
      module_unload_index_locked(mod_idx);
  }
  module_unlock();
}

void *module_make_handle_locked(int idx)
{
  uintptr_t token;

  if(idx < 0 || idx >= ELFLOADER_MAX_MODULES || !g_module_used[idx] || g_modules[idx].generation == 0)
    return 0;

  token = ((uintptr_t)g_modules[idx].generation << 8) | (uintptr_t)(idx + 1);
  token = (token << 1) | (uintptr_t)1u;
  if(token == 0)
    return 0;
  return (void *)token;
}

int module_from_dl_handle_locked(void *handle, int *out_idx, elf_module_t **out_mod)
{
  uintptr_t token = (uintptr_t)handle;
  uintptr_t raw_idx;
  int idx;
  uint32 generation;

  if(out_idx)
    *out_idx = -1;
  if(out_mod)
    *out_mod = 0;

  if(token == 0 || (token & (uintptr_t)1u) == 0)
    return -1;
  token >>= 1;
  raw_idx = token & (uintptr_t)0xffu;
  if(raw_idx == 0)
    return -1;

  idx = (int)(raw_idx - (uintptr_t)1u);
  generation = (uint32)(token >> 8);
  if(idx < 0 || idx >= ELFLOADER_MAX_MODULES || generation == 0)
    return -1;
  if(!g_module_used[idx] || g_modules[idx].generation != generation)
    return -1;

  if(out_idx)
    *out_idx = idx;
  if(out_mod)
    *out_mod = &g_modules[idx];
  return 0;
}

void module_track_dependency_locked(elf_module_t *consumer, elf_module_t *provider)
{
  int pidx;
  uint32 bit;

  if(consumer == 0 || provider == 0 || consumer == provider)
    return;

  pidx = module_index_from_ptr_locked((const void *)provider);
  if(pidx < 0 || pidx >= 32)
    return;

  bit = (uint32)1u << (uint32)pidx;
  if((consumer->deps_mask & bit) != 0)
    return;

  consumer->deps_mask |= bit;
  g_modules[pidx].dependent_count++;
}

void module_unload_index_locked(int idx)
{
  int i;
  uint32 deps;

  if(idx < 0 || idx >= ELFLOADER_MAX_MODULES || !g_module_used[idx])
    return;

  deps = g_modules[idx].deps_mask;
  for(i = 0; i < ELFLOADER_MAX_MODULES && deps != 0; i++){
    uint32 bit = (uint32)1u << (uint32)i;
    if((deps & bit) == 0)
      continue;
    deps &= ~bit;
    if(g_module_used[i] && g_modules[i].dependent_count > 0)
      g_modules[i].dependent_count--;
  }

  g_modules[idx].deps_mask = 0;
  module_reset(&g_modules[idx]);
  g_module_used[idx] = 0;
}

void *alloc_data_mem(size_t sz)
{
  void *p = 0;
#ifdef MALLOC_CAP_SPIRAM
  p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  if(p == 0)
    p = heap_caps_malloc(sz, MALLOC_CAP_8BIT);
  return p;
}

int read_flash_image(uint32 sector, uint32 sector_count, uint8 **out, uint32 *out_size)
{
  uint32 sz;
  uint8 *buf;

  if(out == 0 || out_size == 0 || sector_count == 0)
    return -1;

  if(sector_count > (0xffffffffu / XV6_FLASH_SECTOR_SIZE))
    return -1;
  sz = sector_count * XV6_FLASH_SECTOR_SIZE;

  buf = (uint8 *)alloc_data_mem(sz);
  if(buf == 0)
    return -1;

  if(esp_flash_disk_read(sector, buf, sector_count) != 0){
    heap_caps_free(buf);
    return -1;
  }

  *out = buf;
  *out_size = sz;
  return 0;
}

void set_dlerror(const char *msg)
{
  if(msg && msg[0]){
    strncpy(g_dlerror_msg, msg, sizeof(g_dlerror_msg) - 1);
    g_dlerror_msg[sizeof(g_dlerror_msg) - 1] = 0;
    g_dlerror_set = 1;
  } else {
    g_dlerror_msg[0] = 0;
    g_dlerror_set = 0;
  }
}

int load_image_copy(const void *image, uint32 image_size, uint8 **out_copy)
{
  uint8 *copy;

  if(image == 0 || out_copy == 0 || image_size == 0)
    return -1;

  copy = (uint8 *)alloc_data_mem(image_size);
  if(copy == 0)
    return -1;

  memcpy(copy, image, image_size);
  *out_copy = copy;
  return 0;
}

void module_reset(elf_module_t *m)
{
  int i;

  if(m == 0)
    return;

  for(i = 0; i < m->seg_count; i++){
    if(m->segs[i].mem){
      heap_caps_free(m->segs[i].mem);
      m->segs[i].mem = 0;
    }
    if(m->segs[i].shadow_mem){
      heap_caps_free(m->segs[i].shadow_mem);
      m->segs[i].shadow_mem = 0;
    }
  }

  if(m->image){
    heap_caps_free(m->image);
    m->image = 0;
  }

  memset(m, 0, sizeof(*m));
}
