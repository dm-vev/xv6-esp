#include "loader/elf_loader_internal.h"

#include "esp_cache.h"
#include "esp_err.h"
#include "soc/soc_caps.h"

/*
 * Virtual address mappers are the only sanctioned way to convert ELF virtual addresses
 * into host pointers. Keeping this centralized avoids subtle exec/data mixups.
 */
void *map_vaddr_exec(elf_module_t *m, uint32 vaddr)
{
  int i;

  for(i = 0; i < m->seg_count; i++){
    uint32 off = 0;
    if(!vaddr_offset_in_range(m->segs[i].vaddr, m->segs[i].memsz, vaddr, &off))
      continue;
    return m->segs[i].mem + off;
  }

  return 0;
}

void *map_vaddr_data(elf_module_t *m, uint32 vaddr)
{
  int i;

  for(i = 0; i < m->seg_count; i++){
    uint32 off = 0;
    if(!vaddr_offset_in_range(m->segs[i].vaddr, m->segs[i].memsz, vaddr, &off))
      continue;
    if(m->segs[i].is_exec && m->segs[i].shadow_mem)
      return m->segs[i].shadow_mem + off;
    return m->segs[i].mem + off;
  }

  return 0;
}

static int validate_load_segment(const elf_module_t *m, const elf32_phdr_t *ph)
{
  if(ph->p_memsz == 0)
    return 1;
  if(ph->p_filesz > ph->p_memsz){
    ESP_LOGE(g_elf_loader_tag, "bad segment sizes: filesz=%u memsz=%u", (unsigned)ph->p_filesz,
             (unsigned)ph->p_memsz);
    return -1;
  }
  if(!u32_range_valid(ph->p_offset, ph->p_filesz, m->image_size)){
    ESP_LOGE(g_elf_loader_tag, "segment out of image: off=0x%x filesz=0x%x img=0x%x", (unsigned)ph->p_offset,
             (unsigned)ph->p_filesz, (unsigned)m->image_size);
    return -1;
  }
  if(ph->p_vaddr + ph->p_memsz < ph->p_vaddr){
    ESP_LOGE(g_elf_loader_tag, "segment vaddr overflow: vaddr=0x%x memsz=0x%x", (unsigned)ph->p_vaddr,
             (unsigned)ph->p_memsz);
    return -1;
  }
  return 0;
}

static int parse_segments_contiguous(elf_module_t *m, const elf32_ehdr_t *eh)
{
  int i;
  int load_count = 0;
  int has_exec = 0;
  uint32 min_vaddr = UINT32_MAX;
  uint32 max_vaddr = 0;
  uint32 span;
  uint8 *dst;

  for(i = 0; i < eh->e_phnum; i++){
    const uint32 phoff = eh->e_phoff + (uint32)i * eh->e_phentsize;
    const elf32_phdr_t *ph = (const elf32_phdr_t *)(m->image + phoff);
    int valid;

    if(!u32_range_valid(phoff, sizeof(*ph), m->image_size)){
      ESP_LOGE(g_elf_loader_tag, "program header out of image: off=0x%x", (unsigned)phoff);
      return -1;
    }
    if(ph->p_type != PT_LOAD)
      continue;

    valid = validate_load_segment(m, ph);
    if(valid < 0)
      return -1;
    if(valid > 0)
      continue;

    if(ph->p_vaddr < min_vaddr)
      min_vaddr = ph->p_vaddr;
    if(ph->p_vaddr + ph->p_memsz > max_vaddr)
      max_vaddr = ph->p_vaddr + ph->p_memsz;
    if((ph->p_flags & PF_X) != 0)
      has_exec = 1;
    load_count++;
  }

  if(load_count == 0 || max_vaddr <= min_vaddr)
    return -1;

  span = max_vaddr - min_vaddr;
  if(has_exec)
    dst = (uint8 *)heap_caps_malloc(span, MALLOC_CAP_EXEC);
  else
    dst = (uint8 *)alloc_data_mem(span);
  if(dst == 0){
    ESP_LOGE(g_elf_loader_tag, "contiguous segment alloc failed: memsz=%u exec=%d", (unsigned)span, has_exec);
    return -1;
  }

  memset(dst, 0, span);
  for(i = 0; i < eh->e_phnum; i++){
    const uint32 phoff = eh->e_phoff + (uint32)i * eh->e_phentsize;
    const elf32_phdr_t *ph = (const elf32_phdr_t *)(m->image + phoff);

    if(ph->p_type != PT_LOAD || ph->p_memsz == 0)
      continue;
    memcpy(dst + (ph->p_vaddr - min_vaddr), m->image + ph->p_offset, ph->p_filesz);
  }

  m->segs[0].vaddr = min_vaddr;
  m->segs[0].memsz = span;
  m->segs[0].mem = dst;
  m->segs[0].shadow_mem = 0;
  m->segs[0].is_exec = has_exec ? 1 : 0;
  m->seg_count = 1;
  return 0;
}

int parse_segments(elf_module_t *m, const elf32_ehdr_t *eh)
{
  int i;

  if(eh->e_machine == EM_RISCV)
    return parse_segments_contiguous(m, eh);

  for(i = 0; i < eh->e_phnum; i++){
    const uint32 phoff = eh->e_phoff + (uint32)i * eh->e_phentsize;
    const elf32_phdr_t *ph = (const elf32_phdr_t *)(m->image + phoff);
    uint8 *dst;
    int valid;

    if(!u32_range_valid(phoff, sizeof(*ph), m->image_size)){
      ESP_LOGE(g_elf_loader_tag, "program header out of image: off=0x%x", (unsigned)phoff);
      return -1;
    }
    if(ph->p_type != PT_LOAD)
      continue;
    if(m->seg_count >= (int)(sizeof(m->segs) / sizeof(m->segs[0]))){
      ESP_LOGE(g_elf_loader_tag, "too many PT_LOAD segments");
      return -1;
    }
    valid = validate_load_segment(m, ph);
    if(valid < 0)
      return -1;
    if(valid > 0)
      continue;

    if((ph->p_flags & PF_X) != 0){
      dst = (uint8 *)heap_caps_malloc(ph->p_memsz, MALLOC_CAP_EXEC);
    } else {
      dst = (uint8 *)alloc_data_mem(ph->p_memsz);
    }
    if(dst == 0){
      /*
       * Keep allocation failure reporting side-effect free.
       * Heap capability introspection from this failure path proved unstable
       * under pressure and could panic before returning the original error.
       */
      ESP_LOGE(g_elf_loader_tag, "segment alloc failed: memsz=%u flags=0x%x", (unsigned)ph->p_memsz,
               (unsigned)ph->p_flags);
      return -1;
    }

    memset(dst, 0, ph->p_memsz);
    memcpy(dst, m->image + ph->p_offset, ph->p_filesz);

    m->segs[m->seg_count].vaddr = ph->p_vaddr;
    m->segs[m->seg_count].memsz = ph->p_memsz;
    m->segs[m->seg_count].mem = dst;
    m->segs[m->seg_count].shadow_mem = 0;
    m->segs[m->seg_count].is_exec = ((ph->p_flags & PF_X) != 0) ? 1 : 0;

    if(m->segs[m->seg_count].is_exec){
      m->segs[m->seg_count].shadow_mem = (uint8 *)alloc_data_mem(ph->p_memsz);
      if(m->segs[m->seg_count].shadow_mem == 0){
        /*
         * shadow_mem is an optimization for safe data-view access to exec segments.
         * If unavailable, continue with exec segment only and let map_vaddr_data()
         * fall back to m->segs[i].mem.
         */
        ESP_LOGW(g_elf_loader_tag, "shadow alloc skipped: memsz=%u", (unsigned)ph->p_memsz);
      } else {
        memcpy(m->segs[m->seg_count].shadow_mem, dst, ph->p_memsz);
      }
    }

    m->seg_count++;
  }

  return (m->seg_count > 0) ? 0 : -1;
}

static uintptr_t align_down_ptr(uintptr_t value, size_t align)
{
  return value & ~((uintptr_t)align - 1u);
}

static uintptr_t align_up_ptr(uintptr_t value, size_t align)
{
  return (value + (uintptr_t)align - 1u) & ~((uintptr_t)align - 1u);
}

static void sync_exec_mem(void *addr, size_t size)
{
  esp_err_t err;
  size_t line_size;
  uintptr_t start;
  uintptr_t end;

  if(addr == 0 || size == 0)
    return;

#if SOC_CACHE_WRITEBACK_SUPPORTED
  err = esp_cache_msync(addr, size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  if(err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
    ESP_LOGW(g_elf_loader_tag, "exec dcache sync failed: err=0x%x", (unsigned)err);
#endif

  line_size = esp_cache_get_line_size_by_addr(addr);
  if(line_size != 0 && (line_size & (line_size - 1u)) == 0){
    start = align_down_ptr((uintptr_t)addr, line_size);
    end = align_up_ptr((uintptr_t)addr + size, line_size);
    err = esp_cache_msync((void *)start, end - start, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    if(err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
      ESP_LOGW(g_elf_loader_tag, "exec icache sync failed: err=0x%x", (unsigned)err);
  }

#if defined(__riscv)
  __asm__ __volatile__("fence.i" ::: "memory");
#endif
}

void sync_exec_segments(elf_module_t *m)
{
  int i;

  if(m == 0)
    return;

  for(i = 0; i < m->seg_count; i++){
    if(m->segs[i].is_exec)
      sync_exec_mem(m->segs[i].mem, m->segs[i].memsz);
  }
}

int find_symtab_sections(const elf_module_t *m, const elf32_ehdr_t *eh, const elf32_shdr_t **symtab_sh,
                         const elf32_shdr_t **strtab_sh, const elf32_shdr_t **dynsym_sh,
                         const elf32_shdr_t **dynstr_sh)
{
  int i;
  const elf32_shdr_t *sh;

  sh = (const elf32_shdr_t *)(m->image + eh->e_shoff);

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

int vaddr_is_exec_section(const elf_module_t *m, const elf32_ehdr_t *eh, uint32 vaddr)
{
  const elf32_shdr_t *sh;
  int i;
  uint32 sht_len;

  if(m == 0 || eh == 0 || eh->e_shoff == 0 || eh->e_shnum == 0)
    return 0;

  sht_len = (uint32)eh->e_shnum * (uint32)sizeof(elf32_shdr_t);
  if(!u32_range_valid(eh->e_shoff, sht_len, m->image_size))
    return 0;

  sh = (const elf32_shdr_t *)(m->image + eh->e_shoff);
  for(i = 0; i < eh->e_shnum; i++){
    if((sh[i].sh_flags & SHF_EXECINSTR) == 0 || sh[i].sh_size == 0)
      continue;
    if(vaddr_offset_in_range(sh[i].sh_addr, sh[i].sh_size, vaddr, 0))
      return 1;
  }

  return 0;
}

int section_bounds_valid(const elf_module_t *m, const elf32_shdr_t *sh)
{
  if(m == 0 || sh == 0)
    return 0;
  if(sh->sh_offset > m->image_size)
    return 0;
  if(sh->sh_size > (m->image_size - sh->sh_offset))
    return 0;
  return 1;
}

static int elf_machine_supported(uint16 machine)
{
#if defined(__XTENSA__)
  if(machine == EM_XTENSA)
    return 1;
#endif
#if defined(__riscv)
  if(machine == EM_RISCV)
    return 1;
#endif
  return 0;
}

int validate_elf_header(const elf32_ehdr_t *eh, uint32 size)
{
  uint32 magic;
  uint32 ph_len = 0;
  uint32 sh_len = 0;

  if(size < sizeof(*eh))
    return -1;

  memcpy(&magic, &eh->e_ident[0], sizeof(magic));
  if(magic != ELF_MAGIC)
    return -1;
  if(eh->e_ident[4] != ELFCLASS32 || eh->e_ident[5] != ELFDATA2LSB)
    return -1;
  if(!elf_machine_supported(eh->e_machine))
    return -1;
  if(eh->e_type != ET_EXEC && eh->e_type != ET_DYN)
    return -1;
  if(eh->e_ehsize != sizeof(*eh))
    return -1;

  if(eh->e_phnum > 0){
    if(eh->e_phentsize != sizeof(elf32_phdr_t))
      return -1;
    ph_len = (uint32)eh->e_phnum * (uint32)eh->e_phentsize;
    if(!u32_range_valid(eh->e_phoff, ph_len, size))
      return -1;
  }

  if(eh->e_shnum > 0){
    if(eh->e_shentsize != sizeof(elf32_shdr_t))
      return -1;
    sh_len = (uint32)eh->e_shnum * (uint32)eh->e_shentsize;
    if(!u32_range_valid(eh->e_shoff, sh_len, size))
      return -1;
  }

  return 0;
}
