#include "loader/elf_loader_internal.h"

/*
 * Symbol discovery and resolution policy:
 * local symbol -> globally visible loaded modules -> host export table.
 */
static void *resolve_local_symbol(elf_module_t *m, const elf32_sym_t *sym)
{
  uint8 stt;

  if(sym->st_shndx == SHN_UNDEF)
    return 0;

  stt = (uint8)(sym->st_info & 0x0f);
  if(stt == STT_FUNC)
    return map_vaddr_exec(m, sym->st_value);
  return map_vaddr_data(m, sym->st_value);
}

static void *resolve_host_symbol_exact_locked(const char *name)
{
  int i;

  for(i = g_host_dyn_count - 1; i >= 0; i--){
    if(g_host_dyn_syms[i].addr != 0 && strcmp(name, g_host_dyn_syms[i].name) == 0)
      return g_host_dyn_syms[i].addr;
  }

  for(i = g_host_const_seg_count - 1; i >= 0; i--){
    const elf_host_sym_const_seg_t *seg = &g_host_const_segs[i];
    int j;
    if(seg->syms == 0 || seg->count <= 0)
      continue;
    for(j = seg->count - 1; j >= 0; j--){
      if(seg->syms[j].addr != 0 && strcmp(name, seg->syms[j].name) == 0)
        return seg->syms[j].addr;
    }
  }

  return 0;
}

void *resolve_host_symbol(const char *name)
{
  void *addr = 0;
  char alt[ELFLOADER_NAME_MAX + 2];

  if(name == 0 || name[0] == 0)
    return 0;

  module_lock();
  addr = resolve_host_symbol_exact_locked(name);
  if(addr){
    module_unlock();
    return addr;
  }

  /*
   * Toolchains differ on leading underscore for libc/kernel symbols.
   * Resolve both forms to keep applet ABI stable across build environments.
   */
  if(name[0] == '_'){
    const char *trimmed = name + 1;
    addr = resolve_host_symbol_exact_locked(trimmed);
    if(addr){
      module_unlock();
      return addr;
    }
  } else {
    alt[0] = '_';
    strncpy(alt + 1, name, sizeof(alt) - 2);
    alt[sizeof(alt) - 1] = 0;
    addr = resolve_host_symbol_exact_locked(alt);
    if(addr){
      module_unlock();
      return addr;
    }
  }

  module_unlock();
  return 0;
}

void *module_lookup_symbol(elf_module_t *m, const char *name)
{
  const elf32_ehdr_t *eh;
  const elf32_shdr_t *symtab_sh = 0;
  const elf32_shdr_t *strtab_sh = 0;
  const elf32_shdr_t *dynsym_sh = 0;
  const elf32_shdr_t *dynstr_sh = 0;
  const elf32_shdr_t *sym_sh[2];
  const elf32_shdr_t *str_sh[2];
  int si;

  if(m == 0 || name == 0 || name[0] == 0 || m->image == 0 || m->image_size < sizeof(elf32_ehdr_t))
    return 0;

  eh = (const elf32_ehdr_t *)m->image;
  if(eh->e_shoff == 0 || eh->e_shnum == 0 || eh->e_shoff >= m->image_size)
    return 0;

  find_symtab_sections(m, eh, &symtab_sh, &strtab_sh, &dynsym_sh, &dynstr_sh);

  sym_sh[0] = dynsym_sh;
  str_sh[0] = dynstr_sh;
  sym_sh[1] = symtab_sh;
  str_sh[1] = strtab_sh;

  for(si = 0; si < 2; si++){
    const elf32_sym_t *symtab;
    const char *strtab;
    uint32 nsyms;
    uint32 i;

    if(sym_sh[si] == 0 || str_sh[si] == 0)
      continue;
    if(!section_bounds_valid(m, sym_sh[si]) || !section_bounds_valid(m, str_sh[si]))
      continue;

    symtab = (const elf32_sym_t *)(m->image + sym_sh[si]->sh_offset);
    strtab = (const char *)(m->image + str_sh[si]->sh_offset);
    nsyms = sym_sh[si]->sh_size / sizeof(elf32_sym_t);

    for(i = 0; i < nsyms; i++){
      uint8 bind = symtab[i].st_info >> 4;
      uint8 type = symtab[i].st_info & 0x0f;
      const char *sym_name;
      void *addr;

      if(bind != STB_GLOBAL)
        continue;
      if(type != STT_FUNC && type != STT_OBJECT)
        continue;
      if(symtab[i].st_shndx == SHN_UNDEF)
        continue;
      if(symtab[i].st_name >= str_sh[si]->sh_size)
        continue;

      sym_name = strtab + symtab[i].st_name;
      if(sym_name[0] == 0 || strcmp(sym_name, name) != 0)
        continue;

      if(type == STT_FUNC)
        addr = map_vaddr_exec(m, symtab[i].st_value);
      else
        addr = map_vaddr_data(m, symtab[i].st_value);
      if(addr)
        return addr;
    }
  }

  return 0;
}

void *resolve_module_symbol(const char *name, elf_module_t **out_provider)
{
  int mi;

  if(out_provider)
    *out_provider = 0;

  for(mi = 0; mi < ELFLOADER_MAX_MODULES; mi++){
    void *addr;
    int ei;

    if(!g_module_used[mi])
      continue;
    if(!g_modules[mi].global_visible)
      continue;

    addr = module_lookup_symbol(&g_modules[mi], name);
    if(addr){
      if(out_provider)
        *out_provider = &g_modules[mi];
      return addr;
    }

    for(ei = 0; ei < g_modules[mi].export_count; ei++){
      if(strcmp(name, g_modules[mi].exports[ei].name) == 0){
        if(out_provider)
          *out_provider = &g_modules[mi];
        return g_modules[mi].exports[ei].addr;
      }
    }
  }

  return 0;
}

void *resolve_symbol(elf_module_t *m, const elf32_sym_t *sym, const char *sym_name, elf_module_t **out_provider)
{
  void *addr = resolve_local_symbol(m, sym);

  if(out_provider)
    *out_provider = 0;
  if(addr)
    return addr;

  if(sym_name && sym_name[0]){
    addr = resolve_module_symbol(sym_name, out_provider);
    if(addr)
      return addr;

    addr = resolve_host_symbol(sym_name);
    if(addr)
      return addr;
  }

  return 0;
}

int collect_exports(elf_module_t *m, const elf32_shdr_t *sym_sh, const elf32_shdr_t *str_sh)
{
  uint32 i;
  uint32 nsyms;
  const elf32_sym_t *symtab;
  const char *strtab;

  if(sym_sh == 0 || str_sh == 0)
    return 0;
  if(!section_bounds_valid(m, sym_sh) || !section_bounds_valid(m, str_sh))
    return -1;

  symtab = (const elf32_sym_t *)(m->image + sym_sh->sh_offset);
  strtab = (const char *)(m->image + str_sh->sh_offset);
  nsyms = sym_sh->sh_size / sizeof(elf32_sym_t);

  for(i = 0; i < nsyms; i++){
    uint8 bind = symtab[i].st_info >> 4;
    uint8 type = symtab[i].st_info & 0x0f;
    const char *name;
    void *addr;

    if(bind != STB_GLOBAL || (type != STT_FUNC && type != STT_OBJECT))
      continue;
    if(symtab[i].st_name >= str_sh->sh_size)
      continue;
    if(m->export_count >= ELFLOADER_MAX_EXPORTS)
      break;

    name = strtab + symtab[i].st_name;
    if(name[0] == 0)
      continue;

    if(type == STT_FUNC)
      addr = map_vaddr_exec(m, symtab[i].st_value);
    else
      addr = map_vaddr_data(m, symtab[i].st_value);
    if(addr == 0)
      continue;

    strncpy(m->exports[m->export_count].name, name, ELFLOADER_NAME_MAX - 1);
    m->exports[m->export_count].name[ELFLOADER_NAME_MAX - 1] = 0;
    m->exports[m->export_count].addr = addr;
    m->export_count++;
  }

  return 0;
}
