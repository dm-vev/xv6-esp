#include "loader/elf_loader_internal.h"

/*
 * Relocation writes target both executable and shadow data mappings when needed
 * to preserve consistent views for instruction fetch and host ABI data access.
 */
static int reloc_map_target(elf_module_t *m, const elf32_rela_t *r, uint32 rtype, void **out_target,
                            void **out_target_shadow)
{
  void *target_exec;
  void *target_shadow;
  void *target;

  target_exec = map_vaddr_exec(m, r->r_offset);
  target_shadow = map_vaddr_data(m, r->r_offset);
  target = (target_exec != 0) ? target_exec : target_shadow;
  if(target == 0){
    ESP_LOGE(g_elf_loader_tag, "reloc target map failed: off=0x%x type=%u", (unsigned)r->r_offset,
             (unsigned)rtype);
    return -1;
  }

  *out_target = target;
  *out_target_shadow = target_shadow;
  return 0;
}

static void reloc_write_u32(void *target, void *target_shadow, uint32 val)
{
  *(uint32 *)target = val;
  if(target_shadow && target_shadow != target)
    *(uint32 *)target_shadow = val;
}

static int reloc_write_resolved(elf_module_t *m, const elf32_rela_t *r, uint32 rtype, void *resolved,
                                const char *sym_name)
{
  void *target;
  void *target_shadow;
  uint32 val;

  if(reloc_map_target(m, r, rtype, &target, &target_shadow) != 0)
    return -1;
  if(resolved == 0){
    ESP_LOGE(g_elf_loader_tag, "unresolved symbol: %s", sym_name);
    return -1;
  }

  val = (uint32)(uintptr_t)resolved + (uint32)r->r_addend;
  reloc_write_u32(target, target_shadow, val);
  return 0;
}

static int reloc_write_relative(elf_module_t *m, const elf32_ehdr_t *eh, const elf32_rela_t *r, uint32 rtype)
{
  void *target;
  void *target_shadow;
  uint32 val;

  if(reloc_map_target(m, r, rtype, &target, &target_shadow) != 0)
    return -1;

  val = *(uint32 *)target;
  if(val == 0)
    val = (uint32)r->r_addend;

  {
    void *mapped;
    if(vaddr_is_exec_section(m, eh, val))
      mapped = map_vaddr_exec(m, val);
    else
      mapped = map_vaddr_data(m, val);
    if(mapped == 0)
      mapped = map_vaddr_exec(m, val);
    val = (uint32)(uintptr_t)mapped;
  }

  if(val == 0){
    ESP_LOGE(g_elf_loader_tag, "relative value map failed: val=0x%x add=0x%x", (unsigned)*(uint32 *)target,
             (unsigned)r->r_addend);
    return -1;
  }

  reloc_write_u32(target, target_shadow, val);
  return 0;
}

static int apply_xtensa_relocation(elf_module_t *m, const elf32_ehdr_t *eh, const elf32_rela_t *r, uint32 rtype,
                                   void *resolved, const char *sym_name)
{
  switch(rtype){
  case R_XTENSA_NONE:
    return 0;

  case R_XTENSA_RTLD:
  case R_XTENSA_ASM_EXPAND:
  case R_XTENSA_ASM_SIMPLIFY:
  case R_XTENSA_OP0:
  case R_XTENSA_OP1:
  case R_XTENSA_OP2:
    return 0;

  case R_XTENSA_SLOT0_OP ... R_XTENSA_SLOT14_ALT:
    return 0;

  case R_XTENSA_32:
  case R_XTENSA_PLT:
  case R_XTENSA_GLOB_DAT:
  case R_XTENSA_JMP_SLOT:
    return reloc_write_resolved(m, r, rtype, resolved, sym_name);

  case R_XTENSA_RELATIVE:
    return reloc_write_relative(m, eh, r, rtype);

  default:
    ESP_LOGE(g_elf_loader_tag, "unsupported reloc type: %u", (unsigned)rtype);
    return -1;
  }
}

static int apply_riscv_relocation(elf_module_t *m, const elf32_ehdr_t *eh, const elf32_rela_t *r, uint32 rtype,
                                  void *resolved, const char *sym_name)
{
  switch(rtype){
  case R_RISCV_NONE:
    return 0;

  case R_RISCV_32:
  case R_RISCV_JUMP_SLOT:
    return reloc_write_resolved(m, r, rtype, resolved, sym_name);

  case R_RISCV_RELATIVE:
    return reloc_write_relative(m, eh, r, rtype);

  default:
    ESP_LOGE(g_elf_loader_tag, "unsupported reloc type: %u", (unsigned)rtype);
    return -1;
  }
}

int apply_relocations(elf_module_t *m, const elf32_ehdr_t *eh, const elf32_shdr_t *sym_sh, const elf32_shdr_t *str_sh)
{
  int i;
  const elf32_shdr_t *sh = (const elf32_shdr_t *)(m->image + eh->e_shoff);
  const elf32_sym_t *symtab;
  const char *strtab;
  uint32 nsyms;
  uint32 sym_sh_index;

  if(sym_sh == 0 || str_sh == 0)
    return 0;
  if(!section_bounds_valid(m, sym_sh))
    return -1;
  if(!section_bounds_valid(m, str_sh))
    return -1;

  sym_sh_index = (uint32)(sym_sh - sh);
  symtab = (const elf32_sym_t *)(m->image + sym_sh->sh_offset);
  strtab = (const char *)(m->image + str_sh->sh_offset);
  nsyms = sym_sh->sh_size / sizeof(elf32_sym_t);

  for(i = 0; i < eh->e_shnum; i++){
    uint32 nrela;
    uint32 ri;
    const elf32_rela_t *rela;

    if(sh[i].sh_type != SHT_RELA)
      continue;
    if(sh[i].sh_link != sym_sh_index)
      continue;
    if(!section_bounds_valid(m, &sh[i]))
      return -1;
    if(sh[i].sh_entsize != sizeof(elf32_rela_t))
      return -1;

    rela = (const elf32_rela_t *)(m->image + sh[i].sh_offset);
    nrela = sh[i].sh_size / sh[i].sh_entsize;

    for(ri = 0; ri < nrela; ri++){
      const elf32_rela_t *r = &rela[ri];
      uint32 symi = ELF_R_SYM(r->r_info);
      uint32 rtype = ELF_R_TYPE(r->r_info);
      const elf32_sym_t *sym;
      const char *sym_name;
      void *resolved;
      elf_module_t *provider = 0;

      if(symi >= nsyms){
        ESP_LOGE(g_elf_loader_tag, "reloc sym index out of range: sym=%u nsyms=%u", (unsigned)symi, (unsigned)nsyms);
        return -1;
      }

      sym = &symtab[symi];
      sym_name = (sym->st_name < str_sh->sh_size) ? (strtab + sym->st_name) : "";
      resolved = resolve_symbol(m, sym, sym_name, &provider);
      if(provider && provider != m)
        module_track_dependency_locked(m, provider);

      if(eh->e_machine == EM_XTENSA){
        if(apply_xtensa_relocation(m, eh, r, rtype, resolved, sym_name) != 0)
          return -1;
      } else if(eh->e_machine == EM_RISCV){
        if(apply_riscv_relocation(m, eh, r, rtype, resolved, sym_name) != 0)
          return -1;
      } else {
        ESP_LOGE(g_elf_loader_tag, "unsupported reloc machine: %u", (unsigned)eh->e_machine);
        return -1;
      }
    }
  }

  return 0;
}
