#include "loader/elf_loader.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "platform/esp_flash_disk.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "vfs/xv6fs_ro.h"

#define ELF_MAGIC 0x464c457fU
#define ELFCLASS32 1
#define ELFDATA2LSB 1

#define ET_EXEC 2
#define ET_DYN 3

#define EM_XTENSA 94

#define PT_LOAD 1
#define PF_X 0x1

#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_DYNSYM 11
#define SHF_EXECINSTR 0x4

#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STT_FUNC 2
#define SHN_UNDEF 0

#define ELF_R_SYM(info) ((uint32)((info) >> 8))
#define ELF_R_TYPE(info) ((uint8)((info) & 0xff))

#define R_XTENSA_NONE 0
#define R_XTENSA_32 1
#define R_XTENSA_RTLD 2
#define R_XTENSA_GLOB_DAT 3
#define R_XTENSA_JMP_SLOT 4
#define R_XTENSA_RELATIVE 5
#define R_XTENSA_PLT 6
#define R_XTENSA_OP0 8
#define R_XTENSA_OP1 9
#define R_XTENSA_OP2 10
#define R_XTENSA_ASM_EXPAND 11
#define R_XTENSA_ASM_SIMPLIFY 12
#define R_XTENSA_SLOT0_OP 20
#define R_XTENSA_SLOT14_ALT 49

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
  uint8 *shadow_mem;
  uint8 is_exec;
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
  int global_visible;
  int open_count;
  int active_calls;
  int dependent_count;
  uint32 deps_mask;
  uint32 generation;
};

static const char *TAG = "xv6_elf";
static char g_dlerror_msg[128];
static int g_dlerror_set = 0;

static elf_module_t g_modules[ELFLOADER_MAX_MODULES];
static int g_module_used[ELFLOADER_MAX_MODULES];
static uint32 g_module_generation = 1;

static elf_host_symbol_t g_host_syms[ELFLOADER_MAX_HOST_SYMBOLS];
static int g_host_sym_count;
static SemaphoreHandle_t g_module_mu;

#define ELF_CALL_CTX_MAX XV6_TASK_CTX_CAP
typedef struct {
  TaskHandle_t task;
  elf_module_t *mod;
  jmp_buf jb;
  int jb_valid;
} elf_call_ctx_t;
static elf_call_ctx_t g_call_ctx[ELF_CALL_CTX_MAX];
static SemaphoreHandle_t g_call_ctx_mu;
static void module_reset(elf_module_t *m);
static void *alloc_data_mem(size_t sz);

_Static_assert(ELF_CALL_CTX_MAX >= 8, "ELF_CALL_CTX_MAX too small for concurrent applets");

static void call_ctx_lock(void)
{
  if(g_call_ctx_mu == 0)
    g_call_ctx_mu = xSemaphoreCreateRecursiveMutex();
  if(g_call_ctx_mu)
    (void)xSemaphoreTakeRecursive(g_call_ctx_mu, portMAX_DELAY);
}

static void call_ctx_unlock(void)
{
  if(g_call_ctx_mu)
    (void)xSemaphoreGiveRecursive(g_call_ctx_mu);
}

static int call_ctx_set_current(elf_module_t *mod)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  int i;
  int free_i = -1;

  call_ctx_lock();
  for(i = 0; i < ELF_CALL_CTX_MAX; i++){
    if(g_call_ctx[i].task == self){
      if(mod == 0){
        g_call_ctx[i].task = 0;
        g_call_ctx[i].mod = 0;
        g_call_ctx[i].jb_valid = 0;
      } else {
        g_call_ctx[i].mod = mod;
      }
      call_ctx_unlock();
      return 0;
    }
    if(g_call_ctx[i].task == 0 && free_i < 0)
      free_i = i;
  }
  if(free_i >= 0){
    g_call_ctx[free_i].task = self;
    g_call_ctx[free_i].mod = mod;
    g_call_ctx[free_i].jb_valid = 0;
    call_ctx_unlock();
    return 0;
  }
  call_ctx_unlock();
  return (mod == 0) ? 0 : -1;
}

static elf_module_t *call_ctx_get_current(void)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  int i;
  elf_module_t *m = 0;

  call_ctx_lock();
  for(i = 0; i < ELF_CALL_CTX_MAX; i++){
    if(g_call_ctx[i].task == self){
      m = g_call_ctx[i].mod;
      break;
    }
  }
  call_ctx_unlock();
  return m;
}

static elf_call_ctx_t *call_ctx_get_current_slot(void)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  int i;
  elf_call_ctx_t *slot = 0;

  call_ctx_lock();
  for(i = 0; i < ELF_CALL_CTX_MAX; i++){
    if(g_call_ctx[i].task == self){
      slot = &g_call_ctx[i];
      break;
    }
  }
  call_ctx_unlock();
  return slot;
}

static int u32_range_valid(uint32 off, uint32 len, uint32 size)
{
  if(off > size)
    return 0;
  if(len > size - off)
    return 0;
  return 1;
}

static int vaddr_offset_in_range(uint32 base, uint32 len, uint32 addr, uint32 *out_off)
{
  uint32 off;

  if(len == 0 || addr < base)
    return 0;
  off = addr - base;
  if(off >= len)
    return 0;
  if(out_off)
    *out_off = off;
  return 1;
}

static int module_is_active_in_call_ctx(elf_module_t *mod)
{
  int i;
  int active = 0;

  if(mod == 0)
    return 0;

  call_ctx_lock();
  for(i = 0; i < ELF_CALL_CTX_MAX; i++){
    if(g_call_ctx[i].task != 0 && g_call_ctx[i].mod == mod){
      active = 1;
      break;
    }
  }
  call_ctx_unlock();
  return active;
}

static void module_lock(void)
{
  if(g_module_mu == 0)
    g_module_mu = xSemaphoreCreateRecursiveMutex();
  if(g_module_mu)
    (void)xSemaphoreTakeRecursive(g_module_mu, portMAX_DELAY);
}

static void module_unlock(void)
{
  if(g_module_mu)
    (void)xSemaphoreGiveRecursive(g_module_mu);
}

static int module_index_from_ptr_locked(const void *ptr)
{
  int i;
  for(i = 0; i < ELFLOADER_MAX_MODULES; i++){
    if(g_module_used[i] && ptr == (const void *)&g_modules[i])
      return i;
  }
  return -1;
}

static int module_is_live_locked(const elf_module_t *mod)
{
  return module_index_from_ptr_locked((const void *)mod) >= 0;
}

void elf_loader_task_cleanup_for_handle(void *task_handle)
{
  TaskHandle_t target = (TaskHandle_t)task_handle;
  elf_module_t *mod = 0;
  int i;

  if(target == 0)
    return;

  call_ctx_lock();
  for(i = 0; i < ELF_CALL_CTX_MAX; i++){
    if(g_call_ctx[i].task != target)
      continue;
    mod = g_call_ctx[i].mod;
    memset(&g_call_ctx[i], 0, sizeof(g_call_ctx[i]));
    break;
  }
  call_ctx_unlock();

  if(mod == 0)
    return;

  module_lock();
  if(module_is_live_locked(mod) && mod->active_calls > 0)
    mod->active_calls--;
  module_unlock();
}

static void *module_make_handle_locked(int idx)
{
  uintptr_t token;

  if(idx < 0 || idx >= ELFLOADER_MAX_MODULES || !g_module_used[idx] || g_modules[idx].generation == 0)
    return 0;
  token = ((uintptr_t)g_modules[idx].generation << 8) | (uintptr_t)(idx + 1);
  token = (token << 1) | (uintptr_t)1u;
  if(token == 0)
    return 0;
  return (void *)token;
}

static int module_from_dl_handle_locked(void *handle, int *out_idx, elf_module_t **out_mod)
{
  uintptr_t token = (uintptr_t)handle;
  uintptr_t raw_idx;
  int idx;
  uint32 generation;

  if(out_idx)
    *out_idx = -1;
  if(out_mod)
    *out_mod = 0;

  if(token == 0 || (token & (uintptr_t)1u) == 0)
    return -1;
  token >>= 1;
  raw_idx = token & (uintptr_t)0xffu;
  if(raw_idx == 0)
    return -1;
  idx = (int)(raw_idx - (uintptr_t)1u);
  generation = (uint32)(token >> 8);
  if(idx < 0 || idx >= ELFLOADER_MAX_MODULES || generation == 0)
    return -1;
  if(!g_module_used[idx] || g_modules[idx].generation != generation)
    return -1;

  if(out_idx)
    *out_idx = idx;
  if(out_mod)
    *out_mod = &g_modules[idx];
  return 0;
}

static void module_track_dependency_locked(elf_module_t *consumer, elf_module_t *provider)
{
  int pidx;
  uint32 bit;

  if(consumer == 0 || provider == 0 || consumer == provider)
    return;
  pidx = module_index_from_ptr_locked((const void *)provider);
  if(pidx < 0 || pidx >= 32)
    return;
  bit = (uint32)1u << (uint32)pidx;
  if((consumer->deps_mask & bit) != 0)
    return;
  consumer->deps_mask |= bit;
  g_modules[pidx].dependent_count++;
}

static void module_unload_index_locked(int idx)
{
  int i;
  uint32 deps;

  if(idx < 0 || idx >= ELFLOADER_MAX_MODULES || !g_module_used[idx])
    return;
  deps = g_modules[idx].deps_mask;
  for(i = 0; i < ELFLOADER_MAX_MODULES && deps != 0; i++){
    uint32 bit = (uint32)1u << (uint32)i;
    if((deps & bit) == 0)
      continue;
    deps &= ~bit;
    if(g_module_used[i] && g_modules[i].dependent_count > 0)
      g_modules[i].dependent_count--;
  }
  g_modules[idx].deps_mask = 0;
  module_reset(&g_modules[idx]);
  g_module_used[idx] = 0;
}

static void *alloc_data_mem(size_t sz)
{
  void *p = 0;
#ifdef MALLOC_CAP_SPIRAM
  p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  if(p == 0)
    p = heap_caps_malloc(sz, MALLOC_CAP_8BIT);
  return p;
}

static int read_flash_image(uint32 sector, uint32 sector_count, uint8 **out, uint32 *out_size)
{
  uint32 sz;
  uint8 *buf;

  if(out == 0 || out_size == 0 || sector_count == 0)
    return -1;

  if(sector_count > (0xffffffffu / XV6_FLASH_SECTOR_SIZE))
    return -1;
  sz = sector_count * XV6_FLASH_SECTOR_SIZE;
  buf = (uint8 *)alloc_data_mem(sz);
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

static void set_dlerror(const char *msg)
{
  if(msg && msg[0]){
    strncpy(g_dlerror_msg, msg, sizeof(g_dlerror_msg) - 1);
    g_dlerror_msg[sizeof(g_dlerror_msg) - 1] = 0;
    g_dlerror_set = 1;
  } else {
    g_dlerror_msg[0] = 0;
    g_dlerror_set = 0;
  }
}

static uint32 path_hash32(const char *s)
{
  uint32 h = 2166136261u;
  if(s == 0)
    return h;
  while(*s){
    h ^= (uint8)*s++;
    h *= 16777619u;
  }
  return h;
}

static const char *module_name_from_path(const char *path, char *out, int out_len)
{
  const char *base;
  const char *dot;
  int stem_len;
  int max_stem;
  uint32 hash;
  char hsuf[9];

  if(path == 0 || out == 0 || out_len <= 1)
    return 0;

  base = strrchr(path, '/');
  base = (base != 0) ? (base + 1) : path;
  if(base[0] == 0)
    return 0;

  dot = strrchr(base, '.');
  stem_len = (dot && dot > base) ? (int)(dot - base) : (int)strlen(base);
  if(stem_len <= 0)
    return 0;
  max_stem = out_len - 1 - 1 - 8; /* "<stem>_<hash8>" + NUL */
  if(max_stem < 1)
    return 0;
  if(stem_len > max_stem)
    stem_len = max_stem;

  hash = path_hash32(path);
  snprintf(hsuf, sizeof(hsuf), "%08x", (unsigned)hash);

  memcpy(out, base, (unsigned)stem_len);
  out[stem_len] = '_';
  memcpy(out + stem_len + 1, hsuf, 8u);
  out[stem_len + 1 + 8] = 0;
  return out;
}

static int load_image_copy(const void *image, uint32 image_size, uint8 **out_copy)
{
  uint8 *copy;
  if(image == 0 || out_copy == 0 || image_size == 0)
    return -1;
  copy = (uint8 *)alloc_data_mem(image_size);
  if(copy == 0)
    return -1;
  memcpy(copy, image, image_size);
  *out_copy = copy;
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
    if(m->segs[i].shadow_mem){
      free(m->segs[i].shadow_mem);
      m->segs[i].shadow_mem = 0;
    }
  }
  if(m->image){
    free(m->image);
    m->image = 0;
  }
  memset(m, 0, sizeof(*m));
}

static void *map_vaddr_exec(elf_module_t *m, uint32 vaddr)
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

static void *map_vaddr_data(elf_module_t *m, uint32 vaddr)
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

static int parse_segments(elf_module_t *m, const elf32_ehdr_t *eh)
{
  int i;
  for(i = 0; i < eh->e_phnum; i++){
    const uint32 phoff = eh->e_phoff + (uint32)i * eh->e_phentsize;
    const elf32_phdr_t *ph = (const elf32_phdr_t *)(m->image + phoff);
    uint8 *dst;

    if(!u32_range_valid(phoff, sizeof(*ph), m->image_size)){
      ESP_LOGE(TAG, "program header out of image: off=0x%x", (unsigned)phoff);
      return -1;
    }
    if(ph->p_type != PT_LOAD)
      continue;
    if(m->seg_count >= (int)(sizeof(m->segs) / sizeof(m->segs[0]))){
      ESP_LOGE(TAG, "too many PT_LOAD segments");
      return -1;
    }
    if(ph->p_memsz == 0)
      continue;
    if(ph->p_filesz > ph->p_memsz){
      ESP_LOGE(TAG, "bad segment sizes: filesz=%u memsz=%u", (unsigned)ph->p_filesz, (unsigned)ph->p_memsz);
      return -1;
    }
    if(!u32_range_valid(ph->p_offset, ph->p_filesz, m->image_size)){
      ESP_LOGE(TAG, "segment out of image: off=0x%x filesz=0x%x img=0x%x", (unsigned)ph->p_offset,
               (unsigned)ph->p_filesz, (unsigned)m->image_size);
      return -1;
    }
    if(ph->p_vaddr + ph->p_memsz < ph->p_vaddr){
      ESP_LOGE(TAG, "segment vaddr overflow: vaddr=0x%x memsz=0x%x", (unsigned)ph->p_vaddr, (unsigned)ph->p_memsz);
      return -1;
    }

    if((ph->p_flags & PF_X) != 0){
      dst = (uint8 *)heap_caps_malloc(ph->p_memsz, MALLOC_CAP_EXEC);
    } else {
      dst = (uint8 *)alloc_data_mem(ph->p_memsz);
    }
    if(dst == 0){
      ESP_LOGE(TAG, "segment alloc failed: memsz=%u flags=0x%x", (unsigned)ph->p_memsz, (unsigned)ph->p_flags);
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
        ESP_LOGE(TAG, "shadow alloc failed: memsz=%u", (unsigned)ph->p_memsz);
        free(dst);
        return -1;
      }
      memcpy(m->segs[m->seg_count].shadow_mem, dst, ph->p_memsz);
    }
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

static int vaddr_is_exec_section(const elf_module_t *m, const elf32_ehdr_t *eh, uint32 vaddr)
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

static void *resolve_host_symbol(const char *name)
{
  int i;
  char alt[ELFLOADER_NAME_MAX + 2];

  if(name == 0 || name[0] == 0)
    return 0;

  for(i = g_host_sym_count - 1; i >= 0; i--){
    if(strcmp(name, g_host_syms[i].name) == 0){
      if(g_host_syms[i].addr != 0){
        return g_host_syms[i].addr;
      }
    }
  }

  /* Be tolerant to toolchain symbol naming mismatches: foo vs _foo. */
  if(name[0] == '_'){
    const char *trimmed = name + 1;
    for(i = g_host_sym_count - 1; i >= 0; i--){
      if(strcmp(trimmed, g_host_syms[i].name) == 0 && g_host_syms[i].addr != 0)
        return g_host_syms[i].addr;
    }
  } else {
    alt[0] = '_';
    strncpy(alt + 1, name, sizeof(alt) - 2);
    alt[sizeof(alt) - 1] = 0;
    for(i = g_host_sym_count - 1; i >= 0; i--){
      if(strcmp(alt, g_host_syms[i].name) == 0 && g_host_syms[i].addr != 0)
        return g_host_syms[i].addr;
    }
  }
  return 0;
}

static int section_bounds_valid(const elf_module_t *m, const elf32_shdr_t *sh);
static void *module_lookup_symbol(elf_module_t *m, const char *name);

static void *resolve_module_symbol(const char *name, elf_module_t **out_provider)
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

static void *module_lookup_symbol(elf_module_t *m, const char *name)
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
      if(type != STT_FUNC && type != 1 /* STT_OBJECT */)
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

static void *resolve_symbol(elf_module_t *m, const elf32_sym_t *sym, const char *sym_name, elf_module_t **out_provider)
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

static int section_bounds_valid(const elf_module_t *m, const elf32_shdr_t *sh)
{
  if(m == 0 || sh == 0)
    return 0;
  if(sh->sh_offset > m->image_size)
    return 0;
  if(sh->sh_size > (m->image_size - sh->sh_offset))
    return 0;
  return 1;
}

static int apply_relocations(elf_module_t *m, const elf32_ehdr_t *eh, const elf32_shdr_t *sym_sh, const elf32_shdr_t *str_sh)
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

      if(symi >= nsyms)
      {
        ESP_LOGE(TAG, "reloc sym index out of range: sym=%u nsyms=%u", (unsigned)symi, (unsigned)nsyms);
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
          ESP_LOGE(TAG, "reloc target map failed: off=0x%x type=%u", (unsigned)r->r_offset, (unsigned)rtype);
          return -1;
        }
        if(resolved == 0){
          ESP_LOGE(TAG, "unresolved symbol: %s", sym_name);
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
          ESP_LOGE(TAG, "relative target map failed: off=0x%x", (unsigned)r->r_offset);
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
          ESP_LOGE(TAG, "relative value map failed: val=0x%x add=0x%x", (unsigned)*(uint32 *)target,
                   (unsigned)r->r_addend);
          return -1;
        }
        *(uint32 *)target = val;
        if(target_shadow && target_shadow != target)
          *(uint32 *)target_shadow = val;
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

    if(bind != STB_GLOBAL || (type != STT_FUNC && type != 1 /* STT_OBJECT */))
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

static int validate_elf_header(const elf32_ehdr_t *eh, uint32 size)
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
  if(eh->e_machine != EM_XTENSA)
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

int elf_loader_init(void)
{
  int i;
  module_lock();
  g_host_sym_count = 0;
  g_module_generation = 1;
  memset(g_host_syms, 0, sizeof(g_host_syms));
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
  g_host_sym_count = 0;
  memset(g_host_syms, 0, sizeof(g_host_syms));
  module_unlock();
  return 0;
}

int elf_loader_register_host_symbols(const elf_host_symbol_t *syms, int count)
{
  int i;
  if(syms == 0 || count <= 0)
    return -1;
  module_lock();
  for(i = 0; i < count; i++){
    if(g_host_sym_count >= (int)(sizeof(g_host_syms) / sizeof(g_host_syms[0]))){
      module_unlock();
      return -1;
    }
    g_host_syms[g_host_sym_count++] = syms[i];
  }
  module_unlock();
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

static int elf_module_load_from_image(const char *name, const void *image, uint32 image_size, elf_module_t **out_mod)
{
  elf_module_t *m;
  elf32_ehdr_t *eh;
  // cppcheck-suppress unreadVariable
  const char *fail_reason = 0;
  const elf32_shdr_t *symtab_sh = 0;
  const elf32_shdr_t *strtab_sh = 0;
  const elf32_shdr_t *dynsym_sh = 0;
  const elf32_shdr_t *dynstr_sh = 0;

  if(name == 0 || name[0] == 0 || out_mod == 0)
    return -1;

  module_lock();
  m = elf_module_find(name);
  if(m){
    *out_mod = m;
    module_unlock();
    return 0;
  }

  m = alloc_module_slot(name);
  if(m == 0){
    module_unlock();
    return -1;
  }

  if(load_image_copy(image, image_size, &m->image) != 0){
    fail_reason = "image copy";
    goto fail;
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
    ESP_LOGE(TAG, "entry map failed");
    fail_reason = "entry map";
    goto fail;
  }

  *out_mod = m;
  module_unlock();
  return 0;

fail:
  ESP_LOGE(TAG, "module '%s' load failed: %s", name, fail_reason ? fail_reason : "unknown");
  module_unload_index_locked(module_index_from_ptr_locked((const void *)m));
  module_unlock();
  return -1;
}

int elf_module_load_from_bytes(const char *name, const void *image, uint32 image_size, elf_module_t **out_mod)
{
  return elf_module_load_from_image(name, image, image_size, out_mod);
}

int elf_module_load_from_flash(const char *name, uint32 sector, uint32 sector_count, elf_module_t **out_mod)
{
  uint8 *image = 0;
  uint32 image_size = 0;
  int rc;

  if(read_flash_image(sector, sector_count, &image, &image_size) != 0)
    return -1;
  rc = elf_module_load_from_image(name, image, image_size, out_mod);
  free(image);
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
      if(g_modules[i].open_count > 0 || g_modules[i].active_calls > 0 || g_modules[i].dependent_count > 0){
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
  fn = (main_fn_t)mod->entry_addr;
  if(fn == 0)
    fn = (main_fn_t)elf_module_find_symbol(mod, "main");
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
    ESP_LOGE(TAG, "call context exhausted");
    return -1;
  }
  slot = call_ctx_get_current_slot();
  if(slot == 0){
    (void)call_ctx_set_current(0);
    module_lock();
    if(module_is_live_locked(mod) && mod->active_calls > 0)
      mod->active_calls--;
    module_unlock();
    ESP_LOGE(TAG, "call context slot missing");
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
  if(slot != 0 && slot->jb_valid){
    longjmp(slot->jb, status + 1);
  }
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

void *dlopen(const char *file, int mode)
{
  void *image = 0;
  uint32 image_size = 0;
  elf_module_t *mod = 0;
  void *handle = 0;
  char namebuf[ELFLOADER_NAME_MAX];

  (void)mode;

  if(file == 0 || file[0] == 0){
    set_dlerror("dlopen: bad file");
    return 0;
  }

  if(module_name_from_path(file, namebuf, sizeof(namebuf)) == 0){
    set_dlerror("dlopen: bad module name");
    return 0;
  }

  module_lock();
  mod = elf_module_find(namebuf);
  if(mod){
    int idx = module_index_from_ptr_locked((const void *)mod);
    if(idx >= 0)
      handle = module_make_handle_locked(idx);
    if(handle == 0){
      module_unlock();
      set_dlerror("dlopen: handle state");
      return 0;
    }
    mod->open_count++;
    module_unlock();
    set_dlerror(0);
    return handle;
  }
  module_unlock();

  if(xv6fs_read_file_alloc_path(file, &image, &image_size) != 0 || image == 0){
    set_dlerror("dlopen: file not found");
    return 0;
  }

  if(elf_module_load_from_bytes(namebuf, image, image_size, &mod) != 0){
    free(image);
    set_dlerror("dlopen: load failed");
    return 0;
  }
  free(image);
  module_lock();
  if(!module_is_live_locked(mod)){
    module_unlock();
    set_dlerror("dlopen: load race");
    return 0;
  }
  {
    int idx = module_index_from_ptr_locked((const void *)mod);
    if(idx >= 0)
      handle = module_make_handle_locked(idx);
  }
  if(handle == 0){
    module_unlock();
    set_dlerror("dlopen: handle state");
    return 0;
  }
  mod->open_count++;
  module_unlock();
  (void)elf_module_set_global(mod, 1);
  set_dlerror(0);
  return handle;
}

void *dlsym(void *handle, const char *name)
{
  int idx = -1;
  elf_module_t *mod;
  void *sym;
  if(handle == 0 || name == 0 || name[0] == 0){
    set_dlerror("dlsym: bad args");
    return 0;
  }
  module_lock();
  if(module_from_dl_handle_locked(handle, &idx, &mod) != 0 || mod == 0){
    module_unlock();
    set_dlerror("dlsym: bad handle");
    return 0;
  }
  (void)idx;
  sym = elf_module_find_symbol(mod, name);
  module_unlock();
  if(sym == 0){
    set_dlerror("dlsym: symbol not found");
    return 0;
  }
  set_dlerror(0);
  return sym;
}

int dlclose(void *handle)
{
  int idx;
  elf_module_t *mod;

  module_lock();
  if(module_from_dl_handle_locked(handle, &idx, &mod) != 0 || mod == 0){
    module_unlock();
    set_dlerror("dlclose: bad handle");
    return -1;
  }
  if(mod->open_count <= 0){
    module_unlock();
    set_dlerror("dlclose: not open");
    return -1;
  }
  mod->open_count--;
  if(mod->open_count > 0){
    module_unlock();
    set_dlerror(0);
    return 0;
  }
  if(mod->active_calls > 0 || mod->dependent_count > 0 || module_is_active_in_call_ctx(mod)){
    mod->open_count++;
    module_unlock();
    set_dlerror("dlclose: module busy");
    return -1;
  }
  module_unload_index_locked(idx);
  module_unlock();
  set_dlerror(0);
  return 0;
}

const char *dlerror(void)
{
  const char *msg;
  if(!g_dlerror_set)
    return 0;
  msg = g_dlerror_msg;
  g_dlerror_set = 0;
  return msg;
}
