#include "modules/module_manager_internal.h"

/*
 * Module loading path: verify policy -> dlopen -> descriptor/ABI checks ->
 * host symbol export -> optional init -> publish in slot table.
 */
int kmod_init(const kmod_config_t *cfg)
{
  kmod_lock();
  if(cfg)
    g_cfg = *cfg;
  (void)hostabi_exports_init();
  set_last_error("ok");
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

  if(effective_priority == KMOD_PRIORITY_AUTO)
    effective_priority = desc ? desc->default_priority : 0;

  module_id = alloc_module_id_locked();
  if(module_id < 0){
    (void)dlclose(handle);
    set_last_error("kmod load: no module ids");
    kmod_unlock();
    return -1;
  }

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
