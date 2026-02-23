#ifndef XV6_HOSTABI_EXPORTS_H
#define XV6_HOSTABI_EXPORTS_H

#include "elf_loader.h"

int hostabi_register_exports(const elf_host_symbol_t *syms, int count);

#endif
