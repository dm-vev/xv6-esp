/**
 * @file hostabi_exports.h
 * @brief Host ABI symbol export management interface
 *
 * This header defines the interface for managing exported symbols in the xv6
 * host environment. It provides mechanisms for registering core kernel symbols,
 * module symbols, and resolving symbols at runtime with support for priority-based
 * overrides and extensions.
 */
#ifndef XV6_HOSTABI_EXPORTS_H
#define XV6_HOSTABI_EXPORTS_H

#include "loader/elf_loader.h"

/**
 * @brief Kinds of symbols that can be exported
 *
 * EXTENSION symbols add new functionality without overriding core symbols.
 * OVERRIDE symbols replace core symbols with module-provided implementations.
 */
typedef enum {
  HOSTABI_SYMBOL_EXTENSION = 0,
  HOSTABI_SYMBOL_OVERRIDE = 1,
} hostabi_symbol_kind_t;

/**
 * @brief Symbol entry for module-level exports
 *
 * Contains metadata for a single symbol exported by a loadable module,
 * including its name, address, owning module, priority, and kind.
 */
typedef struct {
  const char *name;       /**< Symbol name (null-terminated) */
  void *addr;             /**< Symbol address in memory */
  int module_id;          /**< ID of the module that owns this symbol */
  int priority;           /**< Priority for resolution (higher = preferred) */
  hostabi_symbol_kind_t kind; /**< Symbol kind (extension or override) */
} hostabi_module_symbol_t;

/**
 * @brief Initialize the export registry
 * @return 0 on success, -1 on failure
 *
 * Must be called before any other export functions. Creates internal
 * synchronization primitives for thread-safe symbol registration.
 */
int hostabi_exports_init(void);

/**
 * @brief Register core kernel symbols
 * @param syms Array of symbol entries to register
 * @param count Number of symbols in the array
 * @return 0 on success, -1 on failure
 *
 * Registers the core set of kernel symbols that form the base symbol table.
 * These symbols have lowest priority in resolution and serve as defaults.
 * @pre syms != NULL && count > 0 && count <= ELFLOADER_MAX_HOST_SYMBOLS
 * @pre Each symbol must have non-null name and address
 */
int hostabi_register_exports(const elf_host_symbol_t *syms, int count);

/**
 * @brief Define core symbols (alias for register_exports)
 * @param syms Array of symbol entries
 * @param count Number of symbols
 * @return 0 on success, -1 on failure
 * @see hostabi_register_exports
 */
int hostabi_export_define_core(const elf_host_symbol_t *syms, int count);

/**
 * @brief Add module symbols to the export registry
 * @param syms Array of module symbol entries
 * @param count Number of symbols in the array
 * @return 0 on success, -1 on failure
 *
 * Registers symbols from a loadable module. Module symbols can override
 * or extend core symbols based on their kind and priority.
 * @pre syms != NULL && count > 0
 * @pre Each symbol must have non-null name, address, and positive module_id
 * @post On failure, all symbols from this call are rolled back
 */
int hostabi_export_add_module(const hostabi_module_symbol_t *syms, int count);

/**
 * @brief Remove all symbols from a module
 * @param module_id ID of the module to remove
 * @return 0 on success, -1 on failure
 *
 * Removes all symbols previously registered by the specified module.
 * @pre module_id > 0
 * @post Core symbols are restored if they were overridden
 */
int hostabi_export_remove_module(int module_id);

/**
 * @brief Resolve a symbol by name
 * @param name Symbol name to resolve
 * @return Symbol address, or NULL if not found
 *
 * Resolves a symbol name to its address using the following priority:
 * 1. Highest-priority override symbol
 * 2. Core symbol (if exists)
 * 3. Highest-priority extension symbol
 * @pre name != NULL && name[0] != '\0'
 */
const void *hostabi_export_resolve(const char *name);

#endif
