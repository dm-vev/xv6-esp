#include "modules/module_manager_internal.h"

/*
 * Runtime operations for already loaded modules: unload/reload/list/verify.
 * All slot table mutations are serialized under kmod_lock.
 */
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
  int signed_tmp = 0;
  int *signed_ptr = signed_ok ? signed_ok : &signed_tmp;

  if(signed_ok)
    *signed_ok = 0;
  if(path == 0 || path[0] == 0)
    return -1;

  kmod_lock();
  rc = parse_signature_for_path(path, signed_ptr);
  kmod_unlock();

  if(rc != 0)
    set_last_error("kmod verify: invalid signature");
  else if(*signed_ptr)
    set_last_error("signed");
  else
    set_last_error("unsigned");
  return rc;
}
