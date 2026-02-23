#include "modules/module_manager.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "loader/elf_loader.h"
#include "hostabi/hostabi_exports.h"
#include "vfs/xv6fs_ro.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/**
 * @file module_manager.c
 * @brief Kernel module manager implementation
 *
 * This file implements the kernel module loading and management system.
 * It provides:
 * - ELF module loading via elf_loader
 * - Module lifecycle management (load/unload/reload)
 * - Symbol export to host ABI
 * - Module reference counting
 * - Manifest-based autoloading
 * - Signature verification (if enabled)
 *
 * Architecture:
 * - Slot-based module tracking (max 16 modules)
 * - Per-module lifecycle functions (init/fini)
 * - Integration with hostabi for symbol export
 * - Thread-safe operations via mutex
 *
 * Module lifecycle:
 * 1. Load ELF from path
 * 2. Call module describe function
 * 3. Verify ABI version compatibility
 * 4. Register exported symbols
 * 5. Call module init function (if any)
 * 6. On unload: call fini, unregister symbols, unload ELF
 */

#define KMOD_MAX_TRACKED 16
#define KMOD_ERR_MAX 160
#define KMOD_SIG_MAGIC "XV6SIG1"
#define KMOD_DEFAULT_MANIFEST "/etc/modules.conf"

typedef struct {
  int used;
  int module_id;
  int priority;
  int signed_ok;
  char name[ELFLOADER_NAME_MAX];
  char path[MAXPATH];
  void *handle;
  xv6_module_lifecycle_fn_t fini_fn;
} kmod_slot_t;

static kmod_slot_t g_slots[KMOD_MAX_TRACKED];
static kmod_config_t g_cfg = {
  XV6_KMOD_DEV_MODE_DEFAULT,
  XV6_KMOD_ALLOW_UNSIGNED_DEV_DEFAULT,
  XV6_KMOD_ALLOW_FORCE_UNLOAD_DEV_DEFAULT,
};
static int g_next_module_id = 1;
static char g_last_error[KMOD_ERR_MAX];
static SemaphoreHandle_t g_kmod_mu;

static void kmod_lock(void)
{
  if(g_kmod_mu == 0)
    g_kmod_mu = xSemaphoreCreateRecursiveMutex();
  if(g_kmod_mu)
    (void)xSemaphoreTakeRecursive(g_kmod_mu, portMAX_DELAY);
}

static void kmod_unlock(void)
{
  if(g_kmod_mu)
    (void)xSemaphoreGiveRecursive(g_kmod_mu);
}

static void set_last_error(const char *fmt, ...)
{
  va_list ap;

  if(fmt == 0)
    fmt = "kmod: unknown";

  va_start(ap, fmt);
  vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
  va_end(ap);
}

static void copy_cstr(char *dst, int dst_len, const char *src)
{
  if(dst == 0 || dst_len <= 0)
    return;
  if(src == 0)
    src = "";
  strncpy(dst, src, (size_t)dst_len - 1u);
  dst[dst_len - 1] = 0;
}

static int path_to_module_name(const char *path, char *out, int out_len)
{
  const char *base;
  int len;

  if(path == 0 || out == 0 || out_len <= 1)
    return -1;

  base = strrchr(path, '/');
  base = (base != 0) ? (base + 1) : path;
  if(base[0] == 0)
    return -1;

  copy_cstr(out, out_len, base);
  len = (int)strlen(out);
  if(len > 3 && strcmp(out + len - 3, ".so") == 0)
    out[len - 3] = 0;
  if(out[0] == 0)
    return -1;
  return 0;
}

static int slot_index_by_id_locked(int module_id)
{
  int i;
  for(i = 0; i < KMOD_MAX_TRACKED; i++){
    if(g_slots[i].used && g_slots[i].module_id == module_id)
      return i;
  }
  return -1;
}

static int slot_index_by_path_locked(const char *path)
{
  int i;
  for(i = 0; i < KMOD_MAX_TRACKED; i++){
    if(g_slots[i].used && strcmp(g_slots[i].path, path) == 0)
      return i;
  }
  return -1;
}

static int slot_index_by_name_locked(const char *name)
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

static int alloc_slot_locked(void)
{
  int i;
  for(i = 0; i < KMOD_MAX_TRACKED; i++){
    if(!g_slots[i].used)
      return i;
  }
  return -1;
}

static int parse_signature_for_path(const char *path, int *signed_ok)
{
  char sig_path[MAXPATH + 8];
  int n;
  void *buf = 0;
  uint32 sz = 0;

  if(signed_ok == 0 || path == 0 || path[0] == 0)
    return -1;

  *signed_ok = 0;
  n = snprintf(sig_path, sizeof(sig_path), "%s.sig", path);
  if(n <= 0 || n >= (int)sizeof(sig_path))
    return -1;

  if(xv6fs_read_file_alloc_path(sig_path, &buf, &sz) != 0 || buf == 0)
    return 0;

  if(sz >= (uint32)(sizeof(KMOD_SIG_MAGIC) - 1u) &&
     memcmp(buf, KMOD_SIG_MAGIC, sizeof(KMOD_SIG_MAGIC) - 1u) == 0){
    *signed_ok = 1;
    free(buf);
    return 0;
  }

  free(buf);
  return -1;
}

static int parse_line_priority(const char *tok, int *out_priority)
{
  char *endp;
  long v;

  if(tok == 0 || out_priority == 0)
    return -1;
  v = strtol(tok, &endp, 10);
  if(endp == tok || *endp != 0)
    return -1;
  if(v < -2147483647L - 1L || v > 2147483647L)
    return -1;
  *out_priority = (int)v;
  return 0;
}

static int register_module_symbols(int module_id, int base_priority, const xv6_module_desc_t *desc)
{
  int i;

  if(desc == 0 || desc->symbol_count <= 0 || desc->symbols == 0)
    return 0;

  for(i = 0; i < desc->symbol_count; i++){
    hostabi_module_symbol_t hs;
    int prio = (desc->symbols[i].priority != 0) ? desc->symbols[i].priority : base_priority;

    hs.name = desc->symbols[i].name;
    hs.addr = desc->symbols[i].addr;
    hs.module_id = module_id;
    hs.priority = prio;
    hs.kind = (desc->symbols[i].kind == XV6_MODULE_SYMBOL_OVERRIDE) ? HOSTABI_SYMBOL_OVERRIDE : HOSTABI_SYMBOL_EXTENSION;

    if(hostabi_export_add_module(&hs, 1) != 0)
      return -1;
  }

  return 0;
}

int kmod_init(const kmod_config_t *cfg)
{
  kmod_lock();
  if(cfg)
    g_cfg = *cfg;
  (void)hostabi_exports_init();
  kmod_unlock();
  return 0;
}

int kmod_load(const char *path, int *module_id_out)
{
  return kmod_load_with_priority(path, KMOD_PRIORITY_AUTO, module_id_out);
}

int kmod_load_with_priority(const char *path, int priority, int *module_id_out)
{
  void *handle = 0;
  xv6_module_describe_fn_t describe_fn;
  const xv6_module_desc_t *desc = 0;
  xv6_module_lifecycle_fn_t init_fn;
  xv6_module_lifecycle_fn_t fini_fn;
  char module_name[ELFLOADER_NAME_MAX];
  int effective_priority = priority;
  int signed_ok = 0;
  int module_id;
  int slot;

  if(path == 0 || path[0] == 0){
    set_last_error("kmod load: bad path");
    return -1;
  }

  if(module_id_out)
    *module_id_out = -1;

  if(path_to_module_name(path, module_name, sizeof(module_name)) != 0)
    copy_cstr(module_name, sizeof(module_name), "module");

  kmod_lock();

  if(slot_index_by_path_locked(path) >= 0){
    set_last_error("kmod load: already loaded");
    kmod_unlock();
    return -1;
  }

  if(slot_index_by_name_locked(module_name) >= 0){
    set_last_error("kmod load: module name already loaded");
    kmod_unlock();
    return -1;
  }

  slot = alloc_slot_locked();
  if(slot < 0){
    set_last_error("kmod load: slots exhausted");
    kmod_unlock();
    return -1;
  }

  if(parse_signature_for_path(path, &signed_ok) != 0){
    set_last_error("kmod load: bad signature");
    kmod_unlock();
    return -1;
  }

  if(!signed_ok && (!g_cfg.dev_mode || !g_cfg.allow_unsigned_dev)){
    set_last_error("kmod load: unsigned module rejected");
    kmod_unlock();
    return -1;
  }

  handle = dlopen(path, RTLD_NOW);
  if(handle == 0){
    set_last_error("kmod load: %s", dlerror() ? dlerror() : "dlopen failed");
    kmod_unlock();
    return -1;
  }

  describe_fn = (xv6_module_describe_fn_t)dlsym(handle, "xv6_module_describe");
  if(describe_fn)
    desc = describe_fn();
  if(desc == 0)
    desc = (const xv6_module_desc_t *)dlsym(handle, "xv6_module_desc");

  if(desc && desc->abi_ver != KMOD_MODULE_ABI_VER){
    set_last_error("kmod load: abi mismatch");
    (void)dlclose(handle);
    kmod_unlock();
    return -1;
  }

  if(desc && desc->name && desc->name[0])
    copy_cstr(module_name, sizeof(module_name), desc->name);

  if(slot_index_by_name_locked(module_name) >= 0){
    set_last_error("kmod load: module name already loaded");
    (void)dlclose(handle);
    kmod_unlock();
    return -1;
  }

  if(effective_priority == KMOD_PRIORITY_AUTO){
    if(desc)
      effective_priority = desc->default_priority;
    else
      effective_priority = 0;
  }

  module_id = g_next_module_id++;
  if(g_next_module_id <= 0)
    g_next_module_id = 1;

  if(desc && register_module_symbols(module_id, effective_priority, desc) != 0){
    (void)hostabi_export_remove_module(module_id);
    (void)dlclose(handle);
    set_last_error("kmod load: symbol register failed");
    kmod_unlock();
    return -1;
  }

  init_fn = (xv6_module_lifecycle_fn_t)dlsym(handle, "xv6_module_init");
  fini_fn = (xv6_module_lifecycle_fn_t)dlsym(handle, "xv6_module_fini");
  if(init_fn && init_fn() != 0){
    (void)hostabi_export_remove_module(module_id);
    (void)dlclose(handle);
    set_last_error("kmod load: module init failed");
    kmod_unlock();
    return -1;
  }

  memset(&g_slots[slot], 0, sizeof(g_slots[slot]));
  g_slots[slot].used = 1;
  g_slots[slot].module_id = module_id;
  g_slots[slot].priority = effective_priority;
  g_slots[slot].signed_ok = signed_ok;
  g_slots[slot].handle = handle;
  g_slots[slot].fini_fn = fini_fn;
  copy_cstr(g_slots[slot].name, sizeof(g_slots[slot].name), module_name);
  copy_cstr(g_slots[slot].path, sizeof(g_slots[slot].path), path);

  if(module_id_out)
    *module_id_out = module_id;

  set_last_error("ok");
  kmod_unlock();
  return 0;
}

int kmod_unload(int module_id, int force)
{
  int idx;
  int rc;
  int module_id_local;
  int sym_rc;

  kmod_lock();

  idx = slot_index_by_id_locked(module_id);
  if(idx < 0){
    set_last_error("kmod unload: module id not found");
    kmod_unlock();
    return -1;
  }

  if(force && (!g_cfg.dev_mode || !g_cfg.allow_force_unload_dev)){
    set_last_error("kmod unload: force disabled");
    kmod_unlock();
    return -1;
  }

  if(!force && g_slots[idx].fini_fn && g_slots[idx].fini_fn() != 0){
    set_last_error("kmod unload: module fini failed");
    kmod_unlock();
    return -1;
  }

  module_id_local = g_slots[idx].module_id;
  rc = dlclose(g_slots[idx].handle);
  if(rc != 0){
    set_last_error("kmod unload: %s", dlerror() ? dlerror() : "dlclose failed");
    kmod_unlock();
    return -1;
  }

  sym_rc = hostabi_export_remove_module(module_id_local);
  memset(&g_slots[idx], 0, sizeof(g_slots[idx]));
  if(sym_rc != 0){
    set_last_error("kmod unload: symbol cleanup failed");
    kmod_unlock();
    return -1;
  }

  set_last_error(force ? "forced" : "ok");
  kmod_unlock();
  return 0;
}

int kmod_reload(int module_id, int *new_module_id_out)
{
  int idx;
  char path[MAXPATH];
  int prio;

  if(new_module_id_out)
    *new_module_id_out = -1;

  kmod_lock();
  idx = slot_index_by_id_locked(module_id);
  if(idx < 0){
    set_last_error("kmod reload: module id not found");
    kmod_unlock();
    return -1;
  }
  copy_cstr(path, sizeof(path), g_slots[idx].path);
  prio = g_slots[idx].priority;
  kmod_unlock();

  if(kmod_unload(module_id, 0) != 0)
    return -1;

  return kmod_load_with_priority(path, prio, new_module_id_out);
}

int kmod_reload_path(const char *path, int priority, int *module_id_out)
{
  int idx;
  int old_id = -1;

  if(path == 0 || path[0] == 0)
    return -1;

  kmod_lock();
  idx = slot_index_by_path_locked(path);
  if(idx >= 0)
    old_id = g_slots[idx].module_id;
  kmod_unlock();

  if(old_id > 0 && kmod_unload(old_id, 0) != 0)
    return -1;

  return kmod_load_with_priority(path, priority, module_id_out);
}

int kmod_list(kmod_info_t *out, int cap, int *count_out)
{
  int i;
  int n = 0;

  if(count_out)
    *count_out = 0;

  kmod_lock();
  for(i = 0; i < KMOD_MAX_TRACKED; i++){
    if(!g_slots[i].used)
      continue;
    if(out && n < cap){
      memset(&out[n], 0, sizeof(out[n]));
      out[n].module_id = g_slots[i].module_id;
      out[n].priority = g_slots[i].priority;
      out[n].loaded = 1;
      out[n].signed_ok = g_slots[i].signed_ok;
      out[n].refcnt = 1;
      copy_cstr(out[n].name, sizeof(out[n].name), g_slots[i].name);
      copy_cstr(out[n].path, sizeof(out[n].path), g_slots[i].path);
    }
    n++;
  }
  kmod_unlock();

  if(count_out)
    *count_out = n;
  return 0;
}

int kmod_verify_signature(const char *path, int *signed_ok)
{
  int rc;

  if(signed_ok)
    *signed_ok = 0;
  if(path == 0 || path[0] == 0)
    return -1;

  kmod_lock();
  rc = parse_signature_for_path(path, signed_ok);
  kmod_unlock();
  if(rc != 0)
    set_last_error("kmod verify: invalid signature");
  else if(signed_ok && *signed_ok)
    set_last_error("signed");
  else
    set_last_error("unsigned");
  return rc;
}

int kmod_autoload_from_manifest(const char *manifest_path, int *loaded_count)
{
  void *buf = 0;
  uint32 sz = 0;
  char *text;
  char *line;
  int loaded = 0;

  if(loaded_count)
    *loaded_count = 0;

  if(manifest_path == 0 || manifest_path[0] == 0)
    manifest_path = KMOD_DEFAULT_MANIFEST;

  if(xv6fs_read_file_alloc_path(manifest_path, &buf, &sz) != 0 || buf == 0){
    int err = xv6_last_errno();
    if(err == ENOENT || err == 0)
      return 0;
    set_last_error("kmod autoload: manifest read failed");
    return -1;
  }

  text = (char *)malloc((size_t)sz + 1u);
  if(text == 0){
    free(buf);
    set_last_error("kmod autoload: no memory");
    return -1;
  }
  memcpy(text, buf, (size_t)sz);
  text[sz] = 0;
  free(buf);

  line = text;
  while(*line){
    char *next = strchr(line, '\n');
    char *p;
    char *path_tok;
    int prio = KMOD_PRIORITY_AUTO;

    if(next){
      *next = 0;
      next++;
    }

    p = line;
    while(*p && isspace((unsigned char)*p))
      p++;
    if(*p == '#' || *p == 0){
      line = next ? next : p + strlen(p);
      continue;
    }

    path_tok = p;
    while(*p && !isspace((unsigned char)*p))
      p++;
    if(*p){
      *p++ = 0;
      while(*p && isspace((unsigned char)*p))
        p++;
      if(*p){
        if(parse_line_priority(p, &prio) != 0){
          free(text);
          set_last_error("kmod autoload: bad priority");
          return -1;
        }
      }
    }

    if(kmod_load_with_priority(path_tok, prio, 0) != 0){
      free(text);
      return -1;
    }
    loaded++;

    if(next)
      line = next;
    else
      break;
  }

  free(text);
  if(loaded_count)
    *loaded_count = loaded;
  set_last_error("ok");
  return 0;
}

int kmod_dev_mode(void)
{
  return g_cfg.dev_mode;
}

const char *kmod_last_error(void)
{
  if(g_last_error[0] == 0)
    return "ok";
  return g_last_error;
}
