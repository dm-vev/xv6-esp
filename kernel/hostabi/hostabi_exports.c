/**
 * @file hostabi_exports.c
 * @brief Host ABI export registration implementation
 *
 * This file provides a simple wrapper around exports_registry functionality.
 * It registers both libc host symbols and ELF loader symbols during initialization.
 */
#include "hostabi/hostabi_exports.h"

extern int ksh_register_libc_host_symbols(void);

/**
 * @brief Register kernel exports
 * @param syms Symbol array (unused in this implementation)
 * @param count Symbol count (unused in this implementation)
 * @return 0 on success, -1 on failure
 *
 * This function registers both libc symbols and ELF loader symbols
 * with the export registry. The actual core symbols are defined elsewhere.
 * @note The syms and count parameters are unused; core symbols are
 *       registered through other mechanisms
 */
int hostabi_register_exports(const elf_host_symbol_t *syms, int count)
{
  int rc = 0;
  (void)syms;
  (void)count;
  if(ksh_register_libc_host_symbols() != 0)
    rc = -1;
  if(elf_loader_register_host_symbols(syms, count) != 0)
    rc = -1;
  return rc;
}
