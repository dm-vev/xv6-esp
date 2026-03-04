#include "modules/module_manager_internal.h"

/*
 * Runtime operations for loaded modules: unload/reload/list/verify.
 * Refcount model: total_ref = ext_refcnt + number_of_dependents.
 */

static int kmod_unload_slot_locked(int idx, int force)
{
  kmod_slot_t slot;
  int open_count = 0;
  int active_calls = 0;
  int dependent_count = 0;
  int in_call_ctx = 0;
  int rc;

  if(idx < 0 || idx >= KMOD_MAX_TRACKED || !g_slots[idx].used)
    return -1;

  slot = g_slots[idx];

  if(elf_loader_handle_refstate(slot.handle, &open_count, &active_calls, &dependent_count, &in_call_ctx) != 0){
    set_last_error("kmod unload: bad handle");
    return -1;
  }

  if(!force && (open_count > 1 || active_calls > 0 || dependent_count > 0 || in_call_ctx)){
    set_last_error("kmod unload: module busy");
    return -1;
  }

  if(!force && slot.fini_fn && slot.fini_fn() != 0){
    set_last_error("kmod unload: module fini failed");
    return -1;
  }

  rc = dlclose(slot.handle);
  if(rc != 0){
    const char *dl_err = dlerror();
    set_last_error("kmod unload: %s", dl_err ? dl_err : "dlclose failed");
    return -1;
  }

  if(hostabi_export_remove_module(slot.module_id) != 0){
    set_last_error("kmod unload: symbol cleanup failed");
    return -1;
  }

  remove_dependencies_from_module_locked(idx);
  remove_all_edges_to_module_locked(idx);
  memset(&g_slots[idx], 0, sizeof(g_slots[idx]));
  return 0;
}

static int kmod_gc_orphans_locked(void)
{
  int progress;

  do {
    int i;
    progress = 0;

    for(i = 0; i < KMOD_MAX_TRACKED; i++){
      if(!g_slots[i].used)
        continue;
      if(g_slots[i].ext_refcnt != 0)
        continue;
      if(dependency_refcnt_locked(i) != 0)
        continue;

      if(kmod_unload_slot_locked(i, 0) != 0)
        return -1;
      progress = 1;
      break;
    }
  } while(progress);

  return 0;
}

int kmod_unload(int module_id, int force)
{
  int idx;
  int incoming_refs;

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

  incoming_refs = dependency_refcnt_locked(idx);

  if(!force){
    if(g_slots[idx].ext_refcnt <= 0){
      set_last_error("kmod unload: no external refs");
      kmod_unlock();
      return -1;
    }

    if(g_slots[idx].ext_refcnt - 1 + incoming_refs > 0){
      g_slots[idx].ext_refcnt--;
      set_last_error("kmod unload: decremented refcnt");
      kmod_unlock();
      return 0;
    }

    if(incoming_refs != 0){
      set_last_error("kmod unload: module busy");
      kmod_unlock();
      return -1;
    }
  }

  if(kmod_unload_slot_locked(idx, force) != 0){
    kmod_unlock();
    return -1;
  }

  if(kmod_gc_orphans_locked() != 0){
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
  if(g_slots[idx].ext_refcnt != 1 || dependency_refcnt_locked(idx) != 0){
    set_last_error("kmod reload: module busy");
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
  if(idx >= 0){
    if(g_slots[idx].ext_refcnt != 1 || dependency_refcnt_locked(idx) != 0){
      set_last_error("kmod reload: module busy");
      kmod_unlock();
      return -1;
    }
    old_id = g_slots[idx].module_id;
  }
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
      out[n].refcnt = total_refcnt_locked(i);
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
