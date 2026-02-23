#ifndef XV6_HOSTABI_EXPORTS_H
#define XV6_HOSTABI_EXPORTS_H

#include "loader/elf_loader.h"

typedef enum {
  HOSTABI_SYMBOL_EXTENSION = 0,
  HOSTABI_SYMBOL_OVERRIDE = 1,
} hostabi_symbol_kind_t;

typedef struct {
  const char *name;
  void *addr;
  int module_id;
  int priority;
  hostabi_symbol_kind_t kind;
} hostabi_module_symbol_t;

int hostabi_exports_init(void);
int hostabi_register_exports(const elf_host_symbol_t *syms, int count);
int hostabi_export_define_core(const elf_host_symbol_t *syms, int count);
int hostabi_export_add_module(const hostabi_module_symbol_t *syms, int count);
int hostabi_export_remove_module(int module_id);
const void *hostabi_export_resolve(const char *name);

#endif
