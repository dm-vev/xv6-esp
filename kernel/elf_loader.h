#ifndef XV6_ELF_LOADER_H
#define XV6_ELF_LOADER_H

#include "types.h"

#define ELFLOADER_MAX_MODULES 32
#define ELFLOADER_MAX_EXPORTS 96
#define ELFLOADER_NAME_MAX 32
#define ELFLOADER_MAX_HOST_SYMBOLS 4096

typedef struct {
  const char *name;
  void *addr;
} elf_host_symbol_t;

typedef struct elf_module elf_module_t;

#ifndef RTLD_LAZY
#define RTLD_LAZY 0x00001
#endif
#ifndef RTLD_NOW
#define RTLD_NOW 0x00002
#endif
#ifndef RTLD_NOLOAD
#define RTLD_NOLOAD 0x00004
#endif
#ifndef RTLD_DEEPBIND
#define RTLD_DEEPBIND 0x00008
#endif

int elf_loader_init(void);
int elf_loader_register_host_symbols(const elf_host_symbol_t *syms, int count);

int elf_module_load_from_bytes(const char *name, const void *image, uint32 image_size, elf_module_t **out_mod);
int elf_module_load_from_flash(const char *name, uint32 sector, uint32 sector_count, elf_module_t **out_mod);
int elf_module_unload(const char *name);
elf_module_t *elf_module_find(const char *name);
int elf_module_set_global(elf_module_t *mod, int global_visible);

void *elf_module_find_symbol(elf_module_t *mod, const char *sym_name);
int elf_module_call0(elf_module_t *mod, const char *sym_name, int *retv);
int elf_module_call_main(elf_module_t *mod, int argc, char **argv, int *retv);
int elf_module_call_main_ex(elf_module_t *mod, int argc, char **argv, char **envp, int *retv);

int elf_module_info(elf_module_t *mod, uint16 *etype, uint16 *machine, uint32 *entry_vaddr, int *nsegs, int *nexports);
void elf_module_list(const char **names, int max_names, int *out_count);
const void *elf_loader_translate_ptr(const void *ptr);
void elf_loader_host_exit(int status);
void elf_loader_task_cleanup_for_handle(void *task_handle);

void *dlopen(const char *file, int mode);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
const char *dlerror(void);

#endif
