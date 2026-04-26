#ifndef XV6_ELF_LOADER_INTERNAL_H
#define XV6_ELF_LOADER_INTERNAL_H

#include "loader/elf_loader.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "platform/esp_flash_disk.h"
#include "vfs/vfs.h"

#define ELF_MAGIC 0x464c457fU
#define ELFCLASS32 1
#define ELFDATA2LSB 1

#define ET_EXEC 2
#define ET_DYN 3

#define EM_XTENSA 94
#define EM_RISCV 243

#define PT_LOAD 1
#define PF_X 0x1

#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_DYNSYM 11
#define SHF_EXECINSTR 0x4

#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STT_OBJECT 1
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

#define R_RISCV_NONE 0
#define R_RISCV_32 1
#define R_RISCV_RELATIVE 3
#define R_RISCV_JUMP_SLOT 5

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

typedef struct {
  const elf_host_symbol_t *syms;
  int count;
} elf_host_sym_const_seg_t;

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

  elf_export_t *exports;
  int export_count;
  int export_cap;
  int global_visible;
  int open_count;
  int active_calls;
  int dependent_count;
  uint32 deps_mask;
  uint32 generation;
};

#define ELF_CALL_CTX_MAX XV6_TASK_CTX_CAP
typedef struct {
  TaskHandle_t task;
  elf_module_t *mod;
  jmp_buf jb;
  int jb_valid;
} elf_call_ctx_t;

extern const char *g_elf_loader_tag;

extern char g_dlerror_msg[128];
extern int g_dlerror_set;

extern elf_module_t g_modules[ELFLOADER_MAX_MODULES];
extern int g_module_used[ELFLOADER_MAX_MODULES];
extern uint32 g_module_generation;

#define ELFLOADER_HOST_CONST_SEG_MAX 8
extern elf_host_sym_const_seg_t g_host_const_segs[ELFLOADER_HOST_CONST_SEG_MAX];
extern int g_host_const_seg_count;
extern elf_host_symbol_t *g_host_dyn_syms;
extern int g_host_dyn_count;
extern int g_host_dyn_cap;

extern SemaphoreHandle_t g_module_mu;
extern elf_call_ctx_t *g_call_ctx;
extern int g_call_ctx_cap;
extern SemaphoreHandle_t g_call_ctx_mu;

void call_ctx_lock(void);
void call_ctx_unlock(void);
int call_ctx_set_current(elf_module_t *mod);
elf_module_t *call_ctx_get_current(void);
elf_call_ctx_t *call_ctx_get_current_slot(void);

int u32_range_valid(uint32 off, uint32 len, uint32 size);
int vaddr_offset_in_range(uint32 base, uint32 len, uint32 addr, uint32 *out_off);

int module_is_active_in_call_ctx(elf_module_t *mod);

void module_lock(void);
void module_unlock(void);

int module_index_from_ptr_locked(const void *ptr);
int module_is_live_locked(const elf_module_t *mod);
void *module_make_handle_locked(int idx);
int module_from_dl_handle_locked(void *handle, int *out_idx, elf_module_t **out_mod);
void module_track_dependency_locked(elf_module_t *consumer, elf_module_t *provider);
void module_unload_index_locked(int idx);

void *alloc_data_mem(size_t sz);
int read_flash_image(uint32 sector, uint32 sector_count, uint8 **out, uint32 *out_size);

void set_dlerror(const char *msg);
int load_image_copy(const void *image, uint32 image_size, uint8 **out_copy);
void module_reset(elf_module_t *m);

elf_module_t *elf_module_find_locked(const char *name);
elf_module_t *alloc_module_slot_locked(const char *name);

void *map_vaddr_exec(elf_module_t *m, uint32 vaddr);
void *map_vaddr_data(elf_module_t *m, uint32 vaddr);

int parse_segments(elf_module_t *m, const elf32_ehdr_t *eh);
void sync_exec_segments(elf_module_t *m);
int find_symtab_sections(const elf_module_t *m, const elf32_ehdr_t *eh, const elf32_shdr_t **symtab_sh,
                         const elf32_shdr_t **strtab_sh, const elf32_shdr_t **dynsym_sh,
                         const elf32_shdr_t **dynstr_sh);
int vaddr_is_exec_section(const elf_module_t *m, const elf32_ehdr_t *eh, uint32 vaddr);
int section_bounds_valid(const elf_module_t *m, const elf32_shdr_t *sh);
int validate_elf_header(const elf32_ehdr_t *eh, uint32 size);

void *module_lookup_symbol(elf_module_t *m, const char *name);
void *resolve_host_symbol(const char *name);
void *resolve_module_symbol(const char *name, elf_module_t **out_provider);
void *resolve_symbol(elf_module_t *m, const elf32_sym_t *sym, const char *sym_name, elf_module_t **out_provider);
int collect_exports(elf_module_t *m, const elf32_shdr_t *sym_sh, const elf32_shdr_t *str_sh);

int apply_relocations(elf_module_t *m, const elf32_ehdr_t *eh, const elf32_shdr_t *sym_sh, const elf32_shdr_t *str_sh);

#endif
