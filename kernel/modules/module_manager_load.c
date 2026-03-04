#include "modules/module_manager_internal.h"

/*
 * Module loading path: verify policy -> dlopen -> descriptor/ABI checks ->
 * dependency wiring -> host symbol export -> optional init -> publish in table.
 */

static int resolve_dependency_slot_locked(const char *dep, int *out_idx)
{
  int idx = -1;

  if(dep == 0 || dep[0] == 0 || out_idx == 0)
    return -1;

  if(strchr(dep, '/') != 0){
    idx = slot_index_by_path_locked(dep);
  } else {
    char path[MAXPATH];
    int n;

    idx = slot_index_by_name_locked(dep);
    if(idx < 0){
      if(strstr(dep, ".so") != 0)
        n = snprintf(path, sizeof(path), "/lib/modules/%s", dep);
      else
        n = snprintf(path, sizeof(path), "/lib/modules/%s.so", dep);
      if(n > 0 && n < (int)sizeof(path))
        idx = slot_index_by_path_locked(path);
    }
    if(idx < 0){
      if(strstr(dep, ".so") != 0)
        n = snprintf(path, sizeof(path), "/lib/%s", dep);
      else
        n = snprintf(path, sizeof(path), "/lib/%s.so", dep);
      if(n > 0 && n < (int)sizeof(path))
        idx = slot_index_by_path_locked(path);
    }
  }

  if(idx < 0)
    return -1;

  *out_idx = idx;
  return 0;
}

static int kmod_load_internal_locked(const char *path, int priority, int as_dependency, int *module_id_out)
{
  void *handle = 0;
  xv6_module_describe_fn_t describe_fn;
  const xv6_module_desc_t *desc = 0;
  xv6_module_lifecycle_fn_t init_fn;
  xv6_module_lifecycle_fn_t fini_fn;
  const char *const *deps = 0;
  int dep_count = 0;
  int dep_slots[KMOD_MAX_TRACKED];
  int dep_slots_count = 0;
  char module_name[ELFLOADER_NAME_MAX];
  int effective_priority = priority;
  int signed_ok = 0;
  int module_id = -1;
  int slot = -1;
  int symbols_registered = 0;
  int init_called = 0;
  int pushed = 0;
  int i;

  if(module_id_out)
    *module_id_out = -1;

  if(path == 0 || path[0] == 0){
    set_last_error("kmod load: bad path");
    return -1;
  }

  slot = slot_index_by_path_locked(path);
  if(slot >= 0){
    if(!as_dependency)
      g_slots[slot].ext_refcnt++;
    if(module_id_out)
      *module_id_out = g_slots[slot].module_id;
    set_last_error("ok");
    return 0;
  }

  if(load_stack_contains_locked(path)){
    set_last_error("kmod load: dependency cycle");
    return -1;
  }
  if(load_stack_push_locked(path) != 0){
    set_last_error("kmod load: dependency stack overflow");
    return -1;
  }
  pushed = 1;

  if(path_to_module_name(path, module_name, sizeof(module_name)) != 0)
    copy_cstr(module_name, sizeof(module_name), "module");

  if(slot_index_by_name_locked(module_name) >= 0){
    set_last_error("kmod load: module name already loaded");
    goto fail;
  }

  slot = alloc_slot_locked();
  if(slot < 0){
    set_last_error("kmod load: slots exhausted");
    goto fail;
  }

  if(parse_signature_for_path(path, &signed_ok) != 0){
    set_last_error("kmod load: bad signature");
    goto fail;
  }

  if(!signed_ok && (!g_cfg.dev_mode || !g_cfg.allow_unsigned_dev)){
    set_last_error("kmod load: unsigned module rejected");
    goto fail;
  }

  handle = dlopen(path, RTLD_NOW);
  if(handle == 0){
    const char *dl_err = dlerror();
    set_last_error("kmod load: %s", dl_err ? dl_err : "dlopen failed");
    goto fail;
  }

  describe_fn = (xv6_module_describe_fn_t)dlsym(handle, "xv6_module_describe");
  if(describe_fn)
    desc = describe_fn();
  if(desc == 0)
    desc = (const xv6_module_desc_t *)dlsym(handle, "xv6_module_desc");

  if(desc && desc->abi_ver != KMOD_MODULE_ABI_VER){
    set_last_error("kmod load: abi mismatch");
    goto fail;
  }

  if(desc && desc->name && desc->name[0])
    copy_cstr(module_name, sizeof(module_name), desc->name);

  if(slot_index_by_name_locked(module_name) >= 0){
    set_last_error("kmod load: module name already loaded");
    goto fail;
  }

  if(effective_priority == KMOD_PRIORITY_AUTO)
    effective_priority = desc ? desc->default_priority : 0;

  module_id = alloc_module_id_locked();
  if(module_id < 0){
    set_last_error("kmod load: no module ids");
    goto fail;
  }

  if(collect_module_dependencies(handle, &deps, &dep_count) != 0){
    set_last_error("kmod load: bad dependency metadata");
    goto fail;
  }
  for(i = 0; i < dep_count; i++){
    int dep_slot;
    int j;

    if(strcmp(deps[i], module_name) == 0 || strcmp(deps[i], path) == 0){
      set_last_error("kmod load: self dependency");
      goto fail;
    }
    if(resolve_dependency_slot_locked(deps[i], &dep_slot) != 0){
      set_last_error("kmod load: unresolved dependency: %s", deps[i]);
      goto fail;
    }

    for(j = 0; j < dep_slots_count; j++){
      if(dep_slots[j] == dep_slot)
        break;
    }
    if(j == dep_slots_count && dep_slots_count < KMOD_MAX_TRACKED)
      dep_slots[dep_slots_count++] = dep_slot;
  }

  if(desc && register_module_symbols(module_id, effective_priority, desc) != 0){
    set_last_error("kmod load: symbol register failed");
    goto fail;
  }
  symbols_registered = 1;

  init_fn = (xv6_module_lifecycle_fn_t)dlsym(handle, "xv6_module_init");
  fini_fn = (xv6_module_lifecycle_fn_t)dlsym(handle, "xv6_module_fini");
  if(init_fn && init_fn() != 0){
    set_last_error("kmod load: module init failed");
    goto fail;
  }
  init_called = 1;

  memset(&g_slots[slot], 0, sizeof(g_slots[slot]));
  g_slots[slot].used = 1;
  g_slots[slot].module_id = module_id;
  g_slots[slot].priority = effective_priority;
  g_slots[slot].signed_ok = signed_ok;
  g_slots[slot].ext_refcnt = as_dependency ? 0 : 1;
  g_slots[slot].deps_mask = 0;
  g_slots[slot].handle = handle;
  g_slots[slot].fini_fn = fini_fn;
  copy_cstr(g_slots[slot].name, sizeof(g_slots[slot].name), module_name);
  copy_cstr(g_slots[slot].path, sizeof(g_slots[slot].path), path);

  for(i = 0; i < dep_slots_count; i++){
    if(add_dependency_edge_locked(slot, dep_slots[i]) != 0){
      set_last_error("kmod load: dependency cycle");
      goto fail_published;
    }
  }

  if(module_id_out)
    *module_id_out = module_id;
  set_last_error("ok");
  load_stack_pop_locked();
  return 0;

fail_published:
  remove_dependencies_from_module_locked(slot);
  remove_all_edges_to_module_locked(slot);
  if(init_called && g_slots[slot].fini_fn)
    (void)g_slots[slot].fini_fn();
  if(g_slots[slot].handle)
    (void)dlclose(g_slots[slot].handle);
  if(symbols_registered)
    (void)hostabi_export_remove_module(module_id);
  memset(&g_slots[slot], 0, sizeof(g_slots[slot]));
  handle = 0;

fail:
  if(handle){
    if(init_called && fini_fn)
      (void)fini_fn();
    if(symbols_registered)
      (void)hostabi_export_remove_module(module_id);
    (void)dlclose(handle);
  }
  if(pushed)
    load_stack_pop_locked();
  return -1;
}

int kmod_init(const kmod_config_t *cfg)
{
  int i;

  kmod_lock();
  if(cfg)
    g_cfg = *cfg;
  (void)hostabi_exports_init();
  g_next_module_id = 1;
  g_load_stack_depth = 0;
  memset(g_slots, 0, sizeof(g_slots));
  for(i = 0; i < KMOD_MAX_TRACKED; i++)
    g_load_stack[i][0] = 0;
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
  int rc;

  kmod_lock();
  rc = kmod_load_internal_locked(path, priority, 0, module_id_out);
  kmod_unlock();
  return rc;
}
