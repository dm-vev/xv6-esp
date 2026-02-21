#include "elf_loader.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_flash_disk.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define ELF_MAGIC 0x464c457fU
#define ELFCLASS32 1
#define ELFDATA2LSB 1

#define ET_EXEC 2
#define ET_DYN 3

#define EM_XTENSA 94

#define PT_LOAD 1

#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_DYNSYM 11

#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STT_FUNC 2
#define SHN_UNDEF 0

#define ELF_R_SYM(info) ((uint32)((info) >> 8))
#define ELF_R_TYPE(info) ((uint8)((info) & 0xff))

#define R_XTENSA_NONE 0
#define R_XTENSA_32 1
#define R_XTENSA_GLOB_DAT 3
#define R_XTENSA_JMP_SLOT 4
#define R_XTENSA_RELATIVE 5

typedef struct __attribute__((packed)) {
  uint8 e_ident[16];
  uint16 e_type;
  uint16 e_machine;
  uint32 e_version;
  uint32 e_entry;
  uint32 e_phoff;
  uint32 e_shoff;
  uint32 e_flags;
  uint16 e_ehsize;
  uint16 e_phentsize;
  uint16 e_phnum;
  uint16 e_shentsize;
  uint16 e_shnum;
  uint16 e_shstrndx;
} elf32_ehdr_t;

typedef struct __attribute__((packed)) {
  uint32 p_type;
  uint32 p_offset;
  uint32 p_vaddr;
  uint32 p_paddr;
  uint32 p_filesz;
  uint32 p_memsz;
  uint32 p_flags;
  uint32 p_align;
} elf32_phdr_t;

typedef struct __attribute__((packed)) {
  uint32 sh_name;
  uint32 sh_type;
  uint32 sh_flags;
  uint32 sh_addr;
  uint32 sh_offset;
  uint32 sh_size;
  uint32 sh_link;
  uint32 sh_info;
  uint32 sh_addralign;
  uint32 sh_entsize;
} elf32_shdr_t;

typedef struct __attribute__((packed)) {
  uint32 st_name;
  uint32 st_value;
  uint32 st_size;
  uint8 st_info;
  uint8 st_other;
  uint16 st_shndx;
} elf32_sym_t;

typedef struct __attribute__((packed)) {
  uint32 r_offset;
  uint32 r_info;
  int32_t r_addend;
} elf32_rela_t;

typedef struct {
  uint32 vaddr;
  uint32 memsz;
  uint8 *mem;
} elf_seg_t;

typedef struct {
  char name[ELFLOADER_NAME_MAX];
  void *addr;
} elf_export_t;

struct elf_module {
  char name[ELFLOADER_NAME_MAX];
  uint16 etype;
  uint16 machine;
  uint32 entry_vaddr;
  void *entry_addr;

  uint8 *image;
  uint32 image_size;

  elf_seg_t segs[16];
  int seg_count;

  elf_export_t exports[ELFLOADER_MAX_EXPORTS];
  int export_count;
};

static const char *TAG = "xv6_elf";

static elf_module_t g_modules[ELFLOADER_MAX_MODULES];
static int g_module_used[ELFLOADER_MAX_MODULES];

static elf_host_symbol_t g_host_syms[256];
static int g_host_sym_count;

static int read_flash_image(uint32 sector, uint32 sector_count, uint8 **out, uint32 *out_size)
{
  uint32 sz;
  uint8 *buf;

  if(out == 0 || out_size == 0 || sector_count == 0)
    return -1;

  sz = sector_count * XV6_FLASH_SECTOR_SIZE;
  buf = (uint8 *)heap_caps_malloc(sz, MALLOC_CAP_8BIT);
  if(buf == 0)
    return -1;

  if(esp_flash_disk_read(sector, buf, sector_count) != 0){
    free(buf);
    return -1;
  }

  *out = buf;
  *out_size = sz;
  return 0;
}

static void module_reset(elf_module_t *m)
{
  int i;
  if(m == 0)
    return;

  for(i = 0; i < m->seg_count; i++){
    if(m->segs[i].mem){
      free(m->segs[i].mem);
      m->segs[i].mem = 0;
    }
  }
  if(m->image){
    free(m->image);
    m->image = 0;
  }
  memset(m, 0, sizeof(*m));
}

static void *map_vaddr(elf_module_t *m, uint32 vaddr)
{
  int i;
  for(i = 0; i < m->seg_count; i++){
    uint32 start = m->segs[i].vaddr;
    uint32 end = start + m->segs[i].memsz;
    if(vaddr >= start && vaddr < end){
      return m->segs[i].mem + (vaddr - start);
    }
  }
  return 0;
}

static int parse_segments(elf_module_t *m, const elf32_ehdr_t *eh)
{
  int i;
  for(i = 0; i < eh->e_phnum; i++){
    const uint32 phoff = eh->e_phoff + (uint32)i * eh->e_phentsize;
    const elf32_phdr_t *ph = (const elf32_phdr_t *)(m->image + phoff);
    uint8 *dst;

    if(ph->p_type != PT_LOAD)
      continue;
    if(m->seg_count >= (int)(sizeof(m->segs) / sizeof(m->segs[0])))
      return -1;
    if(ph->p_memsz == 0)
      continue;
    if(ph->p_filesz > ph->p_memsz)
      return -1;
    if(ph->p_offset + ph->p_filesz > m->image_size)
      return -1;

    dst = (uint8 *)heap_caps_malloc(ph->p_memsz, MALLOC_CAP_8BIT);
    if(dst == 0)
      return -1;

    memset(dst, 0, ph->p_memsz);
    memcpy(dst, m->image + ph->p_offset, ph->p_filesz);

    m->segs[m->seg_count].vaddr = ph->p_vaddr;
    m->segs[m->seg_count].memsz = ph->p_memsz;
    m->segs[m->seg_count].mem = dst;
    m->seg_count++;
  }
  return (m->seg_count > 0) ? 0 : -1;
}

static int find_symtab_sections(const elf_module_t *m, const elf32_ehdr_t *eh, const elf32_shdr_t **symtab_sh,
                                const elf32_shdr_t **strtab_sh, const elf32_shdr_t **dynsym_sh,
                                const elf32_shdr_t **dynstr_sh)
{
  int i;
  const elf32_shdr_t *sh = (const elf32_shdr_t *)(m->image + eh->e_shoff);

  *symtab_sh = 0;
  *strtab_sh = 0;
  *dynsym_sh = 0;
  *dynstr_sh = 0;

  for(i = 0; i < eh->e_shnum; i++){
    if(sh[i].sh_type == SHT_SYMTAB){
      *symtab_sh = &sh[i];
      if(sh[i].sh_link < eh->e_shnum)
        *strtab_sh = &sh[sh[i].sh_link];
    } else if(sh[i].sh_type == SHT_DYNSYM){
      *dynsym_sh = &sh[i];
      if(sh[i].sh_link < eh->e_shnum)
        *dynstr_sh = &sh[sh[i].sh_link];
    }
  }

  return 0;
}

static void *resolve_local_symbol(elf_module_t *m, const elf32_sym_t *sym)
{
  if(sym->st_shndx == SHN_UNDEF)
    return 0;
  return map_vaddr(m, sym->st_value);
}

static void *resolve_host_symbol(const char *name)
{
  int i;
  for(i = g_host_sym_count - 1; i >= 0; i--){
    if(strcmp(name, g_host_syms[i].name) == 0)
      return g_host_syms[i].addr;
  }
  return 0;
}

static void *resolve_module_symbol(const char *name)
{
  int mi;
  for(mi = 0; mi < ELFLOADER_MAX_MODULES; mi++){
    int ei;
    if(!g_module_used[mi])
      continue;
    for(ei = 0; ei < g_modules[mi].export_count; ei++){
      if(strcmp(name, g_modules[mi].exports[ei].name) == 0)
        return g_modules[mi].exports[ei].addr;
    }
  }
  return 0;
}

static void *resolve_symbol(elf_module_t *m, const elf32_sym_t *sym, const char *sym_name)
{
  void *addr = resolve_local_symbol(m, sym);
  if(addr)
    return addr;
  if(sym_name && sym_name[0]){
    addr = resolve_host_symbol(sym_name);
    if(addr)
      return addr;
    addr = resolve_module_symbol(sym_name);
    if(addr)
      return addr;
  }
  return 0;
}

static int apply_relocations(elf_module_t *m, const elf32_ehdr_t *eh, const elf32_shdr_t *sym_sh, const elf32_shdr_t *str_sh)
{
  int i;
  const elf32_shdr_t *sh = (const elf32_shdr_t *)(m->image + eh->e_shoff);
  const elf32_sym_t *symtab;
  const char *strtab;
  uint32 nsyms;

  if(sym_sh == 0 || str_sh == 0)
    return 0;
  if(sym_sh->sh_offset + sym_sh->sh_size > m->image_size)
    return -1;
  if(str_sh->sh_offset + str_sh->sh_size > m->image_size)
    return -1;

  symtab = (const elf32_sym_t *)(m->image + sym_sh->sh_offset);
  strtab = (const char *)(m->image + str_sh->sh_offset);
  nsyms = sym_sh->sh_size / sizeof(elf32_sym_t);

  for(i = 0; i < eh->e_shnum; i++){
    uint32 nrela;
    uint32 ri;
    const elf32_rela_t *rela;

    if(sh[i].sh_type != SHT_RELA)
      continue;
    if(sh[i].sh_offset + sh[i].sh_size > m->image_size)
      return -1;
    if(sh[i].sh_entsize != sizeof(elf32_rela_t) || sh[i].sh_entsize == 0)
      return -1;

    rela = (const elf32_rela_t *)(m->image + sh[i].sh_offset);
    nrela = sh[i].sh_size / sh[i].sh_entsize;

    for(ri = 0; ri < nrela; ri++){
      const elf32_rela_t *r = &rela[ri];
      uint32 symi = ELF_R_SYM(r->r_info);
      uint8 rtype = ELF_R_TYPE(r->r_info);
      const elf32_sym_t *sym;
      const char *sym_name;
      void *target;
      void *resolved;
      uint32 val;

      if(symi >= nsyms)
        return -1;
      sym = &symtab[symi];
      sym_name = (sym->st_name < str_sh->sh_size) ? (strtab + sym->st_name) : "";

      target = map_vaddr(m, r->r_offset);
      if(target == 0)
        return -1;

      resolved = resolve_symbol(m, sym, sym_name);

      switch(rtype){
      case R_XTENSA_NONE:
        break;
      case R_XTENSA_32:
      case R_XTENSA_GLOB_DAT:
      case R_XTENSA_JMP_SLOT:
        if(resolved == 0){
          ESP_LOGE(TAG, "unresolved symbol: %s", sym_name);
          return -1;
        }
        val = (uint32)(uintptr_t)resolved + (uint32)r->r_addend;
        *(uint32 *)target = val;
        break;
      case R_XTENSA_RELATIVE:
        val = (uint32)(uintptr_t)map_vaddr(m, r->r_addend);
        if(val == 0)
          return -1;
        *(uint32 *)target = val;
        break;
      default:
        ESP_LOGE(TAG, "unsupported reloc type: %u", (unsigned)rtype);
        return -1;
      }
    }
  }
  return 0;
}

static int collect_exports(elf_module_t *m, const elf32_shdr_t *sym_sh, const elf32_shdr_t *str_sh)
{
  uint32 i;
  uint32 nsyms;
  const elf32_sym_t *symtab;
  const char *strtab;

  if(sym_sh == 0 || str_sh == 0)
    return 0;

  symtab = (const elf32_sym_t *)(m->image + sym_sh->sh_offset);
  strtab = (const char *)(m->image + str_sh->sh_offset);
  nsyms = sym_sh->sh_size / sizeof(elf32_sym_t);

  for(i = 0; i < nsyms; i++){
    uint8 bind = symtab[i].st_info >> 4;
    uint8 type = symtab[i].st_info & 0x0f;
    const char *name;
    void *addr;

    if(bind != STB_GLOBAL || type != STT_FUNC)
      continue;
    if(symtab[i].st_name >= str_sh->sh_size)
      continue;
    if(m->export_count >= ELFLOADER_MAX_EXPORTS)
      break;

    name = strtab + symtab[i].st_name;
    if(name[0] == 0)
      continue;

    addr = map_vaddr(m, symtab[i].st_value);
    if(addr == 0)
      continue;

    strncpy(m->exports[m->export_count].name, name, ELFLOADER_NAME_MAX - 1);
    m->exports[m->export_count].name[ELFLOADER_NAME_MAX - 1] = 0;
    m->exports[m->export_count].addr = addr;
    m->export_count++;
  }
  return 0;
}

static int validate_elf_header(const elf32_ehdr_t *eh, uint32 size)
{
  uint32 magic;

  if(size < sizeof(*eh))
    return -1;

  memcpy(&magic, &eh->e_ident[0], sizeof(magic));
  if(magic != ELF_MAGIC)
    return -1;
  if(eh->e_ident[4] != ELFCLASS32 || eh->e_ident[5] != ELFDATA2LSB)
    return -1;
  if(eh->e_machine != EM_XTENSA)
    return -1;
  if(eh->e_type != ET_EXEC && eh->e_type != ET_DYN)
    return -1;
  if(eh->e_phoff + (uint32)eh->e_phnum * (uint32)eh->e_phentsize > size)
    return -1;
  if(eh->e_shoff + (uint32)eh->e_shnum * (uint32)eh->e_shentsize > size)
    return -1;

  return 0;
}

int elf_loader_init(void)
{
  int i;
  g_host_sym_count = 0;
  for(i = 0; i < ELFLOADER_MAX_MODULES; i++){
    g_module_used[i] = 0;
    memset(&g_modules[i], 0, sizeof(g_modules[i]));
  }
  return 0;
}

int elf_loader_register_host_symbols(const elf_host_symbol_t *syms, int count)
{
  int i;
  if(syms == 0 || count <= 0)
    return -1;
  for(i = 0; i < count; i++){
    if(g_host_sym_count >= (int)(sizeof(g_host_syms) / sizeof(g_host_syms[0])))
      return -1;
    g_host_syms[g_host_sym_count++] = syms[i];
  }
  return 0;
}

elf_module_t *elf_module_find(const char *name)
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

static elf_module_t *alloc_module_slot(const char *name)
{
  int i;
  for(i = 0; i < ELFLOADER_MAX_MODULES; i++){
    if(!g_module_used[i]){
      g_module_used[i] = 1;
      memset(&g_modules[i], 0, sizeof(g_modules[i]));
      strncpy(g_modules[i].name, name, ELFLOADER_NAME_MAX - 1);
      return &g_modules[i];
    }
  }
  return 0;
}

int elf_module_load_from_flash(const char *name, uint32 sector, uint32 sector_count, elf_module_t **out_mod)
{
  elf_module_t *m;
  elf32_ehdr_t *eh;
  const elf32_shdr_t *symtab_sh = 0;
  const elf32_shdr_t *strtab_sh = 0;
  const elf32_shdr_t *dynsym_sh = 0;
  const elf32_shdr_t *dynstr_sh = 0;

  if(name == 0 || name[0] == 0 || out_mod == 0)
    return -1;

  m = elf_module_find(name);
  if(m){
    *out_mod = m;
    return 0;
  }

  m = alloc_module_slot(name);
  if(m == 0)
    return -1;

  if(read_flash_image(sector, sector_count, &m->image, &m->image_size) != 0)
    goto fail;

  eh = (elf32_ehdr_t *)m->image;
  if(validate_elf_header(eh, m->image_size) != 0)
    goto fail;

  m->etype = eh->e_type;
  m->machine = eh->e_machine;
  m->entry_vaddr = eh->e_entry;

  if(parse_segments(m, eh) != 0)
    goto fail;

  if(find_symtab_sections(m, eh, &symtab_sh, &strtab_sh, &dynsym_sh, &dynstr_sh) != 0)
    goto fail;

  if(apply_relocations(m, eh, symtab_sh, strtab_sh) != 0)
    goto fail;
  if(apply_relocations(m, eh, dynsym_sh, dynstr_sh) != 0)
    goto fail;

  if(collect_exports(m, symtab_sh, strtab_sh) != 0)
    goto fail;
  if(collect_exports(m, dynsym_sh, dynstr_sh) != 0)
    goto fail;

  m->entry_addr = map_vaddr(m, m->entry_vaddr);
  if(m->entry_addr == 0){
    ESP_LOGE(TAG, "entry map failed");
    goto fail;
  }

  *out_mod = m;
  return 0;

fail:
  module_reset(m);
  {
    int i;
    for(i = 0; i < ELFLOADER_MAX_MODULES; i++){
      if(&g_modules[i] == m){
        g_module_used[i] = 0;
        break;
      }
    }
  }
  return -1;
}

int elf_module_unload(const char *name)
{
  int i;
  if(name == 0)
    return -1;
  for(i = 0; i < ELFLOADER_MAX_MODULES; i++){
    if(g_module_used[i] && strcmp(g_modules[i].name, name) == 0){
      module_reset(&g_modules[i]);
      g_module_used[i] = 0;
      return 0;
    }
  }
  return -1;
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
  return 0;
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

int elf_module_call_main(elf_module_t *mod, int argc, char **argv, int *retv)
{
  typedef int (*main_fn_t)(int argc, char **argv);
  main_fn_t fn;

  if(mod == 0 || mod->entry_addr == 0)
    return -1;

  fn = (main_fn_t)mod->entry_addr;
  if(retv)
    *retv = fn(argc, argv);
  else
    (void)fn(argc, argv);
  return 0;
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
  for(i = 0; i < ELFLOADER_MAX_MODULES; i++){
    if(!g_module_used[i])
      continue;
    if(n < max_names){
      names[n] = g_modules[i].name;
      n++;
    }
  }
  if(out_count)
    *out_count = n;
}
