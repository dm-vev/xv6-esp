#include "loader/elf_loader_internal.h"

/*
 * Public loader lifecycle and module execution entry points.
 * This file intentionally contains no low-level relocation internals.
 */
static int host_sym_total_count_locked(void)
{
  int i;
  int total = g_host_dyn_count;

  for(i = 0; i < g_host_const_seg_count; i++){
    const elf_host_sym_const_seg_t *seg = &g_host_const_segs[i];
    int j;
    if(seg->syms == 0 || seg->count <= 0)
      continue;
    for(j = 0; j < seg->count; j++){
      if(seg->syms[j].addr != 0)
        total++;
    }
  }
  return total;
}

static int host_sym_ensure_dyn_capacity_locked(int min_cap)
{
  elf_host_symbol_t *new_syms;
  int new_cap;

  if(min_cap <= g_host_dyn_cap)
    return 0;

  new_cap = (g_host_dyn_cap > 0) ? g_host_dyn_cap : 64;
  while(new_cap < min_cap){
    if(new_cap > (INT32_MAX / 2))
      return -1;
    new_cap *= 2;
  }

  new_syms = (elf_host_symbol_t *)heap_caps_malloc((size_t)new_cap * sizeof(*new_syms), MALLOC_CAP_8BIT);
  if(new_syms == 0)
    return -1;

  if(g_host_dyn_syms && g_host_dyn_count > 0)
    memcpy(new_syms, g_host_dyn_syms, (size_t)g_host_dyn_count * sizeof(*new_syms));

  if(g_host_dyn_syms)
    heap_caps_free(g_host_dyn_syms);
  g_host_dyn_syms = new_syms;
  g_host_dyn_cap = new_cap;
  return 0;
}

int elf_loader_init(void)
{
  int i;

  call_ctx_lock();
  if(g_call_ctx && g_call_ctx_cap > 0)
    memset(g_call_ctx, 0, (size_t)g_call_ctx_cap * sizeof(*g_call_ctx));
  call_ctx_unlock();

  module_lock();
  g_host_const_seg_count = 0;
  g_host_dyn_count = 0;
  if(g_host_dyn_syms){
    heap_caps_free(g_host_dyn_syms);
    g_host_dyn_syms = 0;
    g_host_dyn_cap = 0;
  }
  g_module_generation = 1;
  memset(g_host_const_segs, 0, sizeof(g_host_const_segs));
  for(i = 0; i < ELFLOADER_MAX_MODULES; i++){
    g_module_used[i] = 0;
    memset(&g_modules[i], 0, sizeof(g_modules[i]));
  }
  module_unlock();
  return 0;
}

int elf_loader_reset_host_symbols(void)
{
  module_lock();
  g_host_const_seg_count = 0;
  g_host_dyn_count = 0;
  memset(g_host_const_segs, 0, sizeof(g_host_const_segs));
  module_unlock();
  return 0;
}

int elf_loader_register_host_symbols(const elf_host_symbol_t *syms, int count)
{
  int i;
  int valid_count = 0;

  if(syms == 0 || count <= 0)
    return -1;
  for(i = 0; i < count; i++){
    if(syms[i].name == 0 || syms[i].name[0] == 0)
      return -1;
    if(syms[i].addr != 0)
      valid_count++;
  }
  if(valid_count == 0)
    return 0;

  module_lock();
  if(host_sym_total_count_locked() > (ELFLOADER_MAX_HOST_SYMBOLS - valid_count)){
    module_unlock();
    return -1;
  }
  if(host_sym_ensure_dyn_capacity_locked(g_host_dyn_count + valid_count) != 0){
    module_unlock();
    return -1;
  }
  for(i = 0; i < count; i++){
    if(syms[i].addr == 0)
      continue;
    g_host_dyn_syms[g_host_dyn_count++] = syms[i];
  }
  module_unlock();
  return 0;
}

int elf_loader_register_host_symbols_const(const elf_host_symbol_t *syms, int count)
{
  int i;
  int valid_count = 0;

  if(syms == 0 || count <= 0)
    return -1;
  for(i = 0; i < count; i++){
    if(syms[i].name == 0 || syms[i].name[0] == 0)
      return -1;
    if(syms[i].addr != 0)
      valid_count++;
  }
  if(valid_count == 0)
    return 0;

  module_lock();
  if(g_host_const_seg_count >= ELFLOADER_HOST_CONST_SEG_MAX ||
     host_sym_total_count_locked() > (ELFLOADER_MAX_HOST_SYMBOLS - valid_count)){
    module_unlock();
    return -1;
  }
  g_host_const_segs[g_host_const_seg_count].syms = syms;
  g_host_const_segs[g_host_const_seg_count].count = count;
  g_host_const_seg_count++;
  module_unlock();
  return 0;
}

elf_module_t *elf_module_find_locked(const char *name)
{
  int i;

  if(name == 0)
    return 0;

  for(i = 0; i < ELFLOADER_MAX_MODULES; i++){
    if(g_module_used[i] && strcmp(g_modules[i].name, name) == 0)
      return &g_modules[i];
  }

  return 0;
}

elf_module_t *elf_module_find(const char *name)
{
  elf_module_t *found;

  if(name == 0)
    return 0;

  module_lock();
  found = elf_module_find_locked(name);
  module_unlock();
  return found;
}

elf_module_t *alloc_module_slot_locked(const char *name)
{
  int i;

  for(i = 0; i < ELFLOADER_MAX_MODULES; i++){
    if(!g_module_used[i]){
      uint32 gen = g_module_generation++;
      if(g_module_generation == 0)
        g_module_generation = 1;
      if(gen == 0)
        gen = g_module_generation++;

      g_module_used[i] = 1;
      memset(&g_modules[i], 0, sizeof(g_modules[i]));
      strncpy(g_modules[i].name, name, ELFLOADER_NAME_MAX - 1);
      g_modules[i].generation = gen;
      return &g_modules[i];
    }
  }

  return 0;
}

static int elf_module_load_from_image(const char *name, const void *image, uint32 image_size, int take_ownership,
                                      elf_module_t **out_mod)
{
  elf_module_t *m;
  elf32_ehdr_t *eh;
  const char *fail_reason = 0;
  const elf32_shdr_t *symtab_sh = 0;
  const elf32_shdr_t *strtab_sh = 0;
  const elf32_shdr_t *dynsym_sh = 0;
  const elf32_shdr_t *dynstr_sh = 0;

  if(name == 0 || name[0] == 0 || out_mod == 0)
    return -1;
  *out_mod = 0;

  module_lock();

  m = elf_module_find_locked(name);
  if(m){
    *out_mod = m;
    module_unlock();
    return 0;
  }

  m = alloc_module_slot_locked(name);
  if(m == 0){
    module_unlock();
    if(take_ownership && image)
      heap_caps_free((void *)image);
    return -1;
  }

  if(take_ownership){
    m->image = (uint8 *)image;
    if(m->image == 0 || image_size == 0){
      fail_reason = "image ownership";
      goto fail;
    }
  } else {
    if(load_image_copy(image, image_size, &m->image) != 0){
      fail_reason = "image copy";
      goto fail;
    }
  }
  m->image_size = image_size;

  eh = (elf32_ehdr_t *)m->image;
  if(validate_elf_header(eh, m->image_size) != 0){
    fail_reason = "header";
    goto fail;
  }

  m->etype = eh->e_type;
  m->machine = eh->e_machine;
  m->entry_vaddr = eh->e_entry;

  if(parse_segments(m, eh) != 0){
    fail_reason = "segments";
    goto fail;
  }

  find_symtab_sections(m, eh, &symtab_sh, &strtab_sh, &dynsym_sh, &dynstr_sh);

  if(dynsym_sh && dynstr_sh){
    if(apply_relocations(m, eh, dynsym_sh, dynstr_sh) != 0){
      fail_reason = "dynsym reloc";
      goto fail;
    }
    if(collect_exports(m, dynsym_sh, dynstr_sh) != 0){
      fail_reason = "dynsym exports";
      goto fail;
    }
  } else {
    if(apply_relocations(m, eh, symtab_sh, strtab_sh) != 0){
      fail_reason = "symtab reloc";
      goto fail;
    }
    if(collect_exports(m, symtab_sh, strtab_sh) != 0){
      fail_reason = "symtab exports";
      goto fail;
    }
  }

  m->entry_addr = map_vaddr_exec(m, m->entry_vaddr);
  if(m->entry_vaddr != 0 && m->entry_addr == 0){
    ESP_LOGE(g_elf_loader_tag, "entry map failed");
    fail_reason = "entry map";
    goto fail;
  }

  *out_mod = m;
  module_unlock();
  return 0;

fail:
  ESP_LOGE(g_elf_loader_tag, "module '%s' load failed: %s", name, fail_reason ? fail_reason : "unknown");
  module_unload_index_locked(module_index_from_ptr_locked((const void *)m));
  module_unlock();
  return -1;
}

int elf_module_load_from_bytes(const char *name, const void *image, uint32 image_size, elf_module_t **out_mod)
{
  return elf_module_load_from_image(name, image, image_size, 0, out_mod);
}

int elf_module_load_from_owned_bytes(const char *name, void *image, uint32 image_size, elf_module_t **out_mod)
{
  return elf_module_load_from_image(name, image, image_size, 1, out_mod);
}

int elf_module_load_from_flash(const char *name, uint32 sector, uint32 sector_count, elf_module_t **out_mod)
{
  elf_module_t *m;
  uint8 *image = 0;
  uint32 image_size = 0;
  int rc = -1;

  if(name == 0 || name[0] == 0 || out_mod == 0 || sector_count == 0)
    return -1;
  *out_mod = 0;

  module_lock();
  m = elf_module_find_locked(name);
  if(m){
    *out_mod = m;
    module_unlock();
    return 0;
  }
  module_unlock();

  if(read_flash_image(sector, sector_count, &image, &image_size) != 0)
    return -1;

  rc = elf_module_load_from_owned_bytes(name, image, image_size, out_mod);
  return rc;
}

int elf_module_unload(const char *name)
{
  int i;

  if(name == 0)
    return -1;

  module_lock();
  for(i = 0; i < ELFLOADER_MAX_MODULES; i++){
    if(g_module_used[i] && strcmp(g_modules[i].name, name) == 0){
      if(g_modules[i].open_count > 0 || g_modules[i].active_calls > 0 || g_modules[i].dependent_count > 0 ||
         module_is_active_in_call_ctx(&g_modules[i])){
        module_unlock();
        return -1;
      }
      module_unload_index_locked(i);
      module_unlock();
      return 0;
    }
  }
  module_unlock();
  return -1;
}

int elf_module_set_global(elf_module_t *mod, int global_visible)
{
  if(mod == 0)
    return -1;

  mod->global_visible = global_visible ? 1 : 0;
  return 0;
}

void *elf_module_find_symbol(elf_module_t *mod, const char *sym_name)
{
  int i;

  if(mod == 0 || sym_name == 0)
    return 0;

  for(i = 0; i < mod->export_count; i++){
    if(strcmp(mod->exports[i].name, sym_name) == 0)
      return mod->exports[i].addr;
  }

  return module_lookup_symbol(mod, sym_name);
}

int elf_module_call0(elf_module_t *mod, const char *sym_name, int *retv)
{
  typedef int (*fn0_t)(void);
  fn0_t fn;

  if(mod == 0 || sym_name == 0)
    return -1;

  fn = (fn0_t)elf_module_find_symbol(mod, sym_name);
  if(fn == 0)
    return -1;

  if(retv)
    *retv = fn();
  else
    (void)fn();
  return 0;
}

int elf_module_call_main_ex(elf_module_t *mod, int argc, char **argv, char **envp, int *retv)
{
  typedef int (*main_fn_t)(int argc, char **argv, char **envp);
  main_fn_t fn;
  elf_call_ctx_t *slot;
  int jmp_rc;
  int app_rc = -1;

  if(mod == 0)
    return -1;

  module_lock();
  if(!module_is_live_locked(mod)){
    module_unlock();
    return -1;
  }

  fn = (main_fn_t)elf_module_find_symbol(mod, "main");
  if(fn == 0)
    fn = (main_fn_t)mod->entry_addr;
  if(fn == 0){
    module_unlock();
    return -1;
  }

  mod->active_calls++;
  module_unlock();

  if(call_ctx_set_current(mod) != 0){
    module_lock();
    if(module_is_live_locked(mod) && mod->active_calls > 0)
      mod->active_calls--;
    module_unlock();
    ESP_LOGE(g_elf_loader_tag, "call context exhausted");
    return -1;
  }

  slot = call_ctx_get_current_slot();
  if(slot == 0){
    (void)call_ctx_set_current(0);
    module_lock();
    if(module_is_live_locked(mod) && mod->active_calls > 0)
      mod->active_calls--;
    module_unlock();
    ESP_LOGE(g_elf_loader_tag, "call context slot missing");
    return -1;
  }

  slot->jb_valid = 1;
  jmp_rc = setjmp(slot->jb);
  if(jmp_rc == 0){
    app_rc = fn(argc, argv, envp);
  } else {
    app_rc = jmp_rc - 1;
  }

  slot->jb_valid = 0;
  if(retv)
    *retv = app_rc;

  (void)call_ctx_set_current(0);

  module_lock();
  if(module_is_live_locked(mod) && mod->active_calls > 0)
    mod->active_calls--;
  module_unlock();
  return 0;
}

int elf_module_call_main(elf_module_t *mod, int argc, char **argv, int *retv)
{
  return elf_module_call_main_ex(mod, argc, argv, 0, retv);
}

void elf_loader_host_exit(int status)
{
  elf_call_ctx_t *slot = call_ctx_get_current_slot();
  if(slot != 0 && slot->jb_valid)
    longjmp(slot->jb, status + 1);
  abort();
}

const void *elf_loader_translate_ptr(const void *ptr)
{
  elf_module_t *m = call_ctx_get_current();
  uintptr_t up = (uintptr_t)ptr;
  int i;

  if(ptr == 0 || m == 0)
    return ptr;

  for(i = 0; i < m->seg_count; i++){
    uintptr_t start = (uintptr_t)m->segs[i].mem;
    if(up >= start){
      uintptr_t off = up - start;
      if(off < (uintptr_t)m->segs[i].memsz){
        if(m->segs[i].shadow_mem)
          return m->segs[i].shadow_mem + off;
        return m->segs[i].mem + off;
      }
    }
  }

  /*
   * Some applets pass raw ELF virtual addresses to host ABI calls.
   * Translate those too so string/argv pointers are always readable.
   */
  for(i = 0; i < m->seg_count; i++){
    uintptr_t start = (uintptr_t)m->segs[i].vaddr;
    if(up >= start){
      uintptr_t off = up - start;
      if(off < (uintptr_t)m->segs[i].memsz){
        if(m->segs[i].shadow_mem)
          return m->segs[i].shadow_mem + off;
        return m->segs[i].mem + off;
      }
    }
  }

  return ptr;
}

int elf_module_info(elf_module_t *mod, uint16 *etype, uint16 *machine, uint32 *entry_vaddr, int *nsegs, int *nexports)
{
  if(mod == 0)
    return -1;
  if(etype)
    *etype = mod->etype;
  if(machine)
    *machine = mod->machine;
  if(entry_vaddr)
    *entry_vaddr = mod->entry_vaddr;
  if(nsegs)
    *nsegs = mod->seg_count;
  if(nexports)
    *nexports = mod->export_count;
  return 0;
}

void elf_module_list(const char **names, int max_names, int *out_count)
{
  int i;
  int n = 0;

  if(out_count)
    *out_count = 0;
  if(names == 0 || max_names <= 0)
    return;

  module_lock();
  for(i = 0; i < ELFLOADER_MAX_MODULES; i++){
    if(!g_module_used[i])
      continue;
    if(n < max_names){
      names[n] = g_modules[i].name;
      n++;
    }
  }
  module_unlock();

  if(out_count)
    *out_count = n;
}
