#include "hostabi/hostabi_exports.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define HOSTABI_MODSYM_MAX 128

typedef struct {
  int used;
  int module_id;
  int priority;
  int seq;
  hostabi_symbol_kind_t kind;
  char name[ELFLOADER_NAME_MAX];
  void *addr;
} hostabi_modsym_t;

static const elf_host_symbol_t *g_core_syms;
static int g_core_count;
static hostabi_modsym_t g_mod_syms[HOSTABI_MODSYM_MAX];
static int g_seq = 1;
static SemaphoreHandle_t g_mu;

extern int ksh_register_libc_host_symbols(void);

static void exports_lock(void)
{
  if(g_mu == 0)
    g_mu = xSemaphoreCreateMutex();
  if(g_mu)
    (void)xSemaphoreTake(g_mu, portMAX_DELAY);
}

static void exports_unlock(void)
{
  if(g_mu)
    (void)xSemaphoreGive(g_mu);
}

static void copy_name(char *dst, int dst_len, const char *src)
{
  if(dst == 0 || dst_len <= 0)
    return;
  if(src == 0)
    src = "";
  strncpy(dst, src, (size_t)dst_len - 1u);
  dst[dst_len - 1] = 0;
}

static int modsym_cmp(const hostabi_modsym_t *a, const hostabi_modsym_t *b)
{
  if(a->priority != b->priority)
    return (a->priority < b->priority) ? -1 : 1;
  if(a->seq != b->seq)
    return (a->seq < b->seq) ? -1 : 1;
  return 0;
}

static int core_has_name_locked(const char *name)
{
  int i;

  if(name == 0 || name[0] == 0 || g_core_syms == 0 || g_core_count <= 0)
    return 0;

  for(i = g_core_count - 1; i >= 0; i--){
    if(g_core_syms[i].name && strcmp(name, g_core_syms[i].name) == 0)
      return 1;
  }
  return 0;
}

static int staged_has_name(const elf_host_symbol_t *staged, int n, const char *name)
{
  int i;

  if(staged == 0 || n <= 0 || name == 0 || name[0] == 0)
    return 0;

  for(i = n - 1; i >= 0; i--){
    if(staged[i].name && strcmp(staged[i].name, name) == 0)
      return 1;
  }
  return 0;
}

static int rebuild_locked(void)
{
  int idx[HOSTABI_MODSYM_MAX];
  elf_host_symbol_t staged[HOSTABI_MODSYM_MAX];
  int staged_count = 0;
  int n = 0;
  int i;

  if(elf_loader_reset_host_symbols() != 0)
    return -1;

  if(ksh_register_libc_host_symbols() != 0)
    return -1;

  if(g_core_syms && g_core_count > 0 && elf_loader_register_host_symbols(g_core_syms, g_core_count) != 0)
    return -1;

  for(i = 0; i < HOSTABI_MODSYM_MAX; i++){
    if(!g_mod_syms[i].used)
      continue;
    idx[n++] = i;
  }

  for(i = 1; i < n; i++){
    int k = i;
    while(k > 0 && modsym_cmp(&g_mod_syms[idx[k - 1]], &g_mod_syms[idx[k]]) > 0){
      int t = idx[k - 1];
      idx[k - 1] = idx[k];
      idx[k] = t;
      k--;
    }
  }

  for(i = 0; i < n; i++){
    const hostabi_modsym_t *ms = &g_mod_syms[idx[i]];
    if(ms->kind == HOSTABI_SYMBOL_EXTENSION &&
       (core_has_name_locked(ms->name) || staged_has_name(staged, staged_count, ms->name))){
      continue;
    }
    staged[staged_count].name = ms->name;
    staged[staged_count].addr = ms->addr;
    staged_count++;
  }

  for(i = 0; i < staged_count; i++){
    if(elf_loader_register_host_symbols(&staged[i], 1) != 0)
      return -1;
  }

  return 0;
}

int hostabi_exports_init(void)
{
  exports_lock();
  exports_unlock();
  return 0;
}

int hostabi_export_define_core(const elf_host_symbol_t *syms, int count)
{
  int i;
  int rc;

  if(syms == 0 || count <= 0 || count > ELFLOADER_MAX_HOST_SYMBOLS)
    return -1;

  exports_lock();

  for(i = 0; i < count; i++){
    if(syms[i].name == 0 || syms[i].name[0] == 0 || syms[i].addr == 0){
      exports_unlock();
      return -1;
    }
  }
  g_core_count = count;
  g_core_syms = syms;

  rc = rebuild_locked();
  exports_unlock();
  return rc;
}

int hostabi_register_exports(const elf_host_symbol_t *syms, int count)
{
  return hostabi_export_define_core(syms, count);
}

int hostabi_export_add_module(const hostabi_module_symbol_t *syms, int count)
{
  int i;
  int added[HOSTABI_MODSYM_MAX];
  int added_count = 0;

  if(syms == 0 || count <= 0)
    return -1;

  exports_lock();

  for(i = 0; i < count; i++){
    int slot = -1;
    int j;

    if(syms[i].name == 0 || syms[i].name[0] == 0 || syms[i].addr == 0 || syms[i].module_id <= 0){
      break;
    }

    for(j = 0; j < HOSTABI_MODSYM_MAX; j++){
      if(!g_mod_syms[j].used){
        slot = j;
        break;
      }
    }
    if(slot < 0)
      break;

    g_mod_syms[slot].used = 1;
    g_mod_syms[slot].module_id = syms[i].module_id;
    g_mod_syms[slot].priority = syms[i].priority;
    g_mod_syms[slot].kind = syms[i].kind;
    g_mod_syms[slot].addr = syms[i].addr;
    g_mod_syms[slot].seq = g_seq++;
    if(g_seq <= 0)
      g_seq = 1;
    copy_name(g_mod_syms[slot].name, sizeof(g_mod_syms[slot].name), syms[i].name);
    added[added_count++] = slot;
  }

  if(i != count || rebuild_locked() != 0){
    int j;
    for(j = 0; j < added_count; j++)
      memset(&g_mod_syms[added[j]], 0, sizeof(g_mod_syms[added[j]]));
    (void)rebuild_locked();
    exports_unlock();
    return -1;
  }

  exports_unlock();
  return 0;
}

int hostabi_export_remove_module(int module_id)
{
  int i;
  int changed = 0;
  hostabi_modsym_t prev[HOSTABI_MODSYM_MAX];

  if(module_id <= 0)
    return -1;

  exports_lock();
  memcpy(prev, g_mod_syms, sizeof(prev));
  for(i = 0; i < HOSTABI_MODSYM_MAX; i++){
    if(g_mod_syms[i].used && g_mod_syms[i].module_id == module_id){
      memset(&g_mod_syms[i], 0, sizeof(g_mod_syms[i]));
      changed = 1;
    }
  }

  if(changed && rebuild_locked() != 0){
    memcpy(g_mod_syms, prev, sizeof(g_mod_syms));
    (void)rebuild_locked();
    exports_unlock();
    return -1;
  }

  exports_unlock();
  return 0;
}

const void *hostabi_export_resolve(const char *name)
{
  int i;
  const void *best_override = 0;
  const void *best_extension = 0;
  const void *core_addr = 0;
  int best_priority = -2147483647;
  int best_seq = -2147483647;
  int ext_priority = -2147483647;
  int ext_seq = -2147483647;

  if(name == 0 || name[0] == 0)
    return 0;

  exports_lock();

  for(i = 0; i < HOSTABI_MODSYM_MAX; i++){
    if(!g_mod_syms[i].used || g_mod_syms[i].addr == 0)
      continue;
    if(strcmp(name, g_mod_syms[i].name) != 0)
      continue;

    if(g_mod_syms[i].kind == HOSTABI_SYMBOL_OVERRIDE){
      if(g_mod_syms[i].priority > best_priority ||
         (g_mod_syms[i].priority == best_priority && g_mod_syms[i].seq > best_seq)){
        best_priority = g_mod_syms[i].priority;
        best_seq = g_mod_syms[i].seq;
        best_override = g_mod_syms[i].addr;
      }
    } else {
      if(g_mod_syms[i].priority > ext_priority ||
         (g_mod_syms[i].priority == ext_priority && g_mod_syms[i].seq > ext_seq)){
        ext_priority = g_mod_syms[i].priority;
        ext_seq = g_mod_syms[i].seq;
        best_extension = g_mod_syms[i].addr;
      }
    }
  }

  for(i = g_core_count - 1; i >= 0; i--){
    if(g_core_syms && g_core_syms[i].addr != 0 && strcmp(name, g_core_syms[i].name) == 0){
      core_addr = g_core_syms[i].addr;
      break;
    }
  }

  if(best_override != 0){
    exports_unlock();
    return best_override;
  }

  if(core_addr != 0){
    exports_unlock();
    return core_addr;
  }

  exports_unlock();
  return best_extension;
}
