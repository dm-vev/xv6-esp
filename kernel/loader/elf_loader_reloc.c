#include "loader/elf_loader_internal.h"

/*
 * Relocation writes target both executable and shadow data mappings when needed
 * to preserve consistent views for instruction fetch and host ABI data access.
 */
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
      uint8 rtype = ELF_R_TYPE(r->r_info);
      const elf32_sym_t *sym;
      const char *sym_name;
      void *target = 0;
      void *target_exec = 0;
      void *target_shadow = 0;
      void *resolved;
      elf_module_t *provider = 0;
      uint32 val;

      if(symi >= nsyms){
        ESP_LOGE(g_elf_loader_tag, "reloc sym index out of range: sym=%u nsyms=%u", (unsigned)symi, (unsigned)nsyms);
        return -1;
      }

      sym = &symtab[symi];
      sym_name = (sym->st_name < str_sh->sh_size) ? (strtab + sym->st_name) : "";
      resolved = resolve_symbol(m, sym, sym_name, &provider);
      if(provider && provider != m)
        module_track_dependency_locked(m, provider);

      switch(rtype){
      case R_XTENSA_NONE:
        break;

      case R_XTENSA_RTLD:
      case R_XTENSA_ASM_EXPAND:
      case R_XTENSA_ASM_SIMPLIFY:
      case R_XTENSA_OP0:
      case R_XTENSA_OP1:
      case R_XTENSA_OP2:
        break;

      case R_XTENSA_SLOT0_OP ... R_XTENSA_SLOT14_ALT:
        break;

      case R_XTENSA_32:
      case R_XTENSA_PLT:
      case R_XTENSA_GLOB_DAT:
      case R_XTENSA_JMP_SLOT:
        target_exec = map_vaddr_exec(m, r->r_offset);
        target_shadow = map_vaddr_data(m, r->r_offset);
        target = (target_exec != 0) ? target_exec : target_shadow;
        if(target == 0){
          ESP_LOGE(g_elf_loader_tag, "reloc target map failed: off=0x%x type=%u", (unsigned)r->r_offset,
                   (unsigned)rtype);
          return -1;
        }
        if(resolved == 0){
          ESP_LOGE(g_elf_loader_tag, "unresolved symbol: %s", sym_name);
          return -1;
        }

        val = (uint32)(uintptr_t)resolved + (uint32)r->r_addend;
        *(uint32 *)target = val;
        if(target_shadow && target_shadow != target)
          *(uint32 *)target_shadow = val;
        break;

      case R_XTENSA_RELATIVE:
        target_exec = map_vaddr_exec(m, r->r_offset);
        target_shadow = map_vaddr_data(m, r->r_offset);
        target = (target_exec != 0) ? target_exec : target_shadow;
        if(target == 0){
          ESP_LOGE(g_elf_loader_tag, "relative target map failed: off=0x%x", (unsigned)r->r_offset);
          return -1;
        }

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

        *(uint32 *)target = val;
        if(target_shadow && target_shadow != target)
          *(uint32 *)target_shadow = val;
        break;

      default:
        ESP_LOGE(g_elf_loader_tag, "unsupported reloc type: %u", (unsigned)rtype);
        return -1;
      }
    }
  }

  return 0;
}
