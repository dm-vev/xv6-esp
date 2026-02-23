#include "hostabi_exports.h"

extern int ksh_register_libc_host_symbols(void);

int hostabi_register_exports(const elf_host_symbol_t *syms, int count)
{
  int rc = 0;
  if(ksh_register_libc_host_symbols() != 0)
    rc = -1;
  if(elf_loader_register_host_symbols(syms, count) != 0)
    rc = -1;
  return rc;
}
