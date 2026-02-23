/**
 * @file exports_registry.c
 * @brief Implementation of host ABI symbol export registry
 *
 * This file implements the symbol export system that manages core kernel symbols
 * and module-provided symbols. It provides symbol resolution with support for:
 * - Core symbols (lowest priority, always available)
 * - Extension symbols (add new functionality)
 * - Override symbols (replace core implementations)
 *
 * The registry uses priority-based resolution where higher priority symbols
 * are preferred when multiple symbols with the same name exist.
 */
#include "hostabi/hostabi_exports.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define HOSTABI_MODSYM_MAX 128

/**
 * @brief Internal structure for module-level symbol entries
 *
 * Stores metadata for each registered module symbol including ownership,
 * priority, sequence number, kind, and address.
 */
typedef struct {
  int used;       /**< Whether this slot is in use */
  int module_id;  /**< ID of the owning module */
  int priority;   /**< Resolution priority (higher = preferred) */
  int seq;        /**< Sequence number for tie-breaking */
  hostabi_symbol_kind_t kind; /**< Symbol kind (extension/override) */
  char name[ELFLOADER_NAME_MAX]; /**< Symbol name */
  void *addr;     /**< Symbol address */
} hostabi_modsym_t;

static const elf_host_symbol_t *g_core_syms; /**< Core symbol table */
static int g_core_count;                       /**< Number of core symbols */
static hostabi_modsym_t g_mod_syms[HOSTABI_MODSYM_MAX]; /**< Module symbols */
static int g_seq = 1;                          /**< Global sequence counter */
static SemaphoreHandle_t g_mu;                 /**< Export registry lock */

extern int ksh_register_libc_host_symbols(void);

/**
 * @brief Acquire the export registry lock
 *
 * Creates the mutex on first call if needed, then acquires it.
 * Uses a lazy initialization pattern to avoid static initialization order issues.
 */
static void exports_lock(void)
{
  if(g_mu == 0)
    g_mu = xSemaphoreCreateMutex();
  if(g_mu)
    (void)xSemaphoreTake(g_mu, portMAX_DELAY);
}

/**
 * @brief Release the export registry lock
 */
static void exports_unlock(void)
{
  if(g_mu)
    (void)xSemaphoreGive(g_mu);
}

/**
 * @brief Safely copy a string with bounds checking
 * @param dst Destination buffer
 * @param dst_len Size of destination buffer
 * @param src Source string (can be NULL)
 *
 * Copies at most dst_len-1 characters and always null-terminates.
 * Treats NULL src as empty string.
 */
static void copy_name(char *dst, int dst_len, const char *src)
{
  if(dst == 0 || dst_len <= 0)
    return;
  if(src == 0)
    src = "";
  strncpy(dst, src, (size_t)dst_len - 1u);
  dst[dst_len - 1] = 0;
}

/**
 * @brief Compare two module symbols for sorting
 * @param a First symbol
 * @param b Second symbol
 * @return -1 if a < b, 1 if a > b, 0 if equal
 *
 * Comparison is by priority first (higher priority first), then by
 * sequence number (higher sequence first).
 */
static int modsym_cmp(const hostabi_modsym_t *a, const hostabi_modsym_t *b)
{
  if(a->priority != b->priority)
    return (a->priority < b->priority) ? -1 : 1;
  if(a->seq != b->seq)
    return (a->seq < b->seq) ? -1 : 1;
  return 0;
}

/**
 * @brief Check if core symbols contain a name
 * @param name Symbol name to search for
 * @return 1 if found, 0 if not
 *
 * Searches backwards through core symbols to find a matching name.
 * The backwards iteration means newer entries are checked first.
 */
static int core_has_name_locked(const char *name)
{
  int i;

  if(name == 0 || name[0] == 0 || g_core_syms == 0 || g_core_count <= 0)
    return 0;

  for(i = g_core_count - 1; i >= 0; i--){
    if(g_core_syms[i].name && strcmp(name, g_core_syms[i].name) == 0)
      return 1;
  }
  return 0;
}

/**
 * @brief Check if staged symbols contain a name
 * @param staged Array of staged symbols
 * @param n Number of staged symbols
 * @param name Symbol name to search for
 * @return 1 if found, 0 if not
 *
 * Searches backwards through staged symbols to find a matching name.
 * Used to detect duplicate extensions during symbol registration.
 */
static int staged_has_name(const elf_host_symbol_t *staged, int n, const char *name)
{
  int i;

  if(staged == 0 || n <= 0 || name == 0 || name[0] == 0)
    return 0;

  for(i = n - 1; i >= 0; i--){
    if(staged[i].name && strcmp(staged[i].name, name) == 0)
      return 1;
  }
  return 0;
}

/**
 * @brief Rebuild the ELF loader's host symbol table
 * @return 0 on success, -1 on failure
 *
 * This is the core function that synchronizes the internal module symbol
 * list with the ELF loader's symbol table. It:
 * 1. Resets the ELF loader's table
 * 2. Re-registers libc symbols
 * 3. Re-registers core symbols
 * 4. Sorts module symbols by priority
 * 5. Filters out extensions
 * 6. duplicate Registers remaining symbols with ELF loader
 *
 * Must be called while holding the export lock.
 */
static int rebuild_locked(void)
{
  int idx[HOSTABI_MODSYM_MAX];
  elf_host_symbol_t staged[HOSTABI_MODSYM_MAX];
  int staged_count = 0;
  int n = 0;
  int i;

  if(elf_loader_reset_host_symbols() != 0)
    return -1;

  if(ksh_register_libc_host_symbols() != 0)
    return -1;

  if(g_core_syms && g_core_count > 0 && elf_loader_register_host_symbols(g_core_syms, g_core_count) != 0)
    return -1;

  /* Collect indices of all used module symbol slots */
  for(i = 0; i < HOSTABI_MODSYM_MAX; i++){
    if(!g_mod_syms[i].used)
      continue;
    idx[n++] = i;
  }

  /* Sort symbols by priority using insertion sort */
  for(i = 1; i < n; i++){
    int k = i;
    while(k > 0 && modsym_cmp(&g_mod_syms[idx[k - 1]], &g_mod_syms[idx[k]]) > 0){
      int t = idx[k - 1];
      idx[k - 1] = idx[k];
      idx[k] = t;
      k--;
    }
  }

  /* Build staged list, skipping duplicate extensions */
  for(i = 0; i < n; i++){
    const hostabi_modsym_t *ms = &g_mod_syms[idx[i]];
    if(ms->kind == HOSTABI_SYMBOL_EXTENSION &&
       (core_has_name_locked(ms->name) || staged_has_name(staged, staged_count, ms->name))){
      continue;
    }
    staged[staged_count].name = ms->name;
    staged[staged_count].addr = ms->addr;
    staged_count++;
  }

  /* Register staged symbols with ELF loader */
  for(i = 0; i < staged_count; i++){
    if(elf_loader_register_host_symbols(&staged[i], 1) != 0)
      return -1;
  }

  return 0;
}

/**
 * @brief Initialize the export registry
 * @return 0 always
 *
 * Performs lazy initialization of the registry lock. Safe to call multiple times.
 */
int hostabi_exports_init(void)
{
  exports_lock();
  exports_unlock();
  return 0;
}

/**
 * @brief Define the core symbol table
 * @param syms Array of core symbol entries
 * @param count Number of symbols
 * @return 0 on success, -1 on failure
 *
 * Sets the core symbol table and triggers a rebuild. Core symbols are
 * the base set of symbols always available in the system.
 * @pre syms != NULL && count > 0 && count <= ELFLOADER_MAX_HOST_SYMBOLS
 * @pre Each symbol must have non-null name and address
 */
int hostabi_export_define_core(const elf_host_symbol_t *syms, int count)
{
  int i;
  int rc;

  if(syms == 0 || count <= 0 || count > ELFLOADER_MAX_HOST_SYMBOLS)
    return -1;

  exports_lock();

  /* Validate all symbols before accepting */
  for(i = 0; i < count; i++){
    if(syms[i].name == 0 || syms[i].name[0] == 0 || syms[i].addr == 0){
      exports_unlock();
      return -1;
    }
  }
  g_core_count = count;
  g_core_syms = syms;

  rc = rebuild_locked();
  exports_unlock();
  return rc;
}

/**
 * @brief Register core symbols (alias)
 * @see hostabi_export_define_core
 */
int hostabi_register_exports(const elf_host_symbol_t *syms, int count)
{
  return hostabi_export_define_core(syms, count);
}

/**
 * @brief Add module symbols to the registry
 * @param syms Array of module symbol entries
 * @param count Number of symbols
 * @return 0 on success, -1 on failure
 *
 * Registers symbols from a loadable module. Each symbol is assigned a
 * sequence number for tie-breaking when priorities are equal.
 * @pre syms != NULL && count > 0
 * @post On failure, all added symbols are rolled back
 */
int hostabi_export_add_module(const hostabi_module_symbol_t *syms, int count)
{
  int i;
  int added[HOSTABI_MODSYM_MAX];
  int added_count = 0;

  if(syms == 0 || count <= 0)
    return -1;

  exports_lock();

  /* Allocate slots and populate symbol data */
  for(i = 0; i < count; i++){
    int slot = -1;
    int j;

    if(syms[i].name == 0 || syms[i].name[0] == 0 || syms[i].addr == 0 || syms[i].module_id <= 0){
      break;
    }

    /* Find free slot */
    for(j = 0; j < HOSTABI_MODSYM_MAX; j++){
      if(!g_mod_syms[j].used){
        slot = j;
        break;
      }
    }
    if(slot < 0)
      break;

    g_mod_syms[slot].used = 1;
    g_mod_syms[slot].module_id = syms[i].module_id;
    g_mod_syms[slot].priority = syms[i].priority;
    g_mod_syms[slot].kind = syms[i].kind;
    g_mod_syms[slot].addr = syms[i].addr;
    g_mod_syms[slot].seq = g_seq++;
    if(g_seq <= 0)
      g_seq = 1;
    copy_name(g_mod_syms[slot].name, sizeof(g_mod_syms[slot].name), syms[i].name);
    added[added_count++] = slot;
  }

  /* If any symbol failed validation, rollback and fail */
  if(i != count || rebuild_locked() != 0){
    int j;
    for(j = 0; j < added_count; j++)
      memset(&g_mod_syms[added[j]], 0, sizeof(g_mod_syms[added[j]]));
    (void)rebuild_locked();
    exports_unlock();
    return -1;
  }

  exports_unlock();
  return 0;
}

/**
 * @brief Remove all symbols from a module
 * @param module_id ID of module to remove
 * @return 0 on success, -1 on failure
 *
 * Removes all symbols that were registered by the specified module.
 * If rebuild fails, restores the previous state.
 * @pre module_id > 0
 */
int hostabi_export_remove_module(int module_id)
{
  int i;
  int changed = 0;
  hostabi_modsym_t prev[HOSTABI_MODSYM_MAX];

  if(module_id <= 0)
    return -1;

  exports_lock();
  /* Save current state for rollback */
  memcpy(prev, g_mod_syms, sizeof(prev));
  /* Mark all symbols from this module as unused */
  for(i = 0; i < HOSTABI_MODSYM_MAX; i++){
    if(g_mod_syms[i].used && g_mod_syms[i].module_id == module_id){
      memset(&g_mod_syms[i], 0, sizeof(g_mod_syms[i]));
      changed = 1;
    }
  }

  /* Rebuild and rollback on failure */
  if(changed && rebuild_locked() != 0){
    memcpy(g_mod_syms, prev, sizeof(g_mod_syms));
    (void)rebuild_locked();
    exports_unlock();
    return -1;
  }

  exports_unlock();
  return 0;
}

/**
 * @brief Resolve a symbol by name
 * @param name Symbol name to resolve
 * @return Symbol address, or NULL if not found
 *
 * Resolution priority:
 * 1. Highest-priority OVERRIDE symbol
 * 2. Core symbol (if exists)
 * 3. Highest-priority EXTENSION symbol
 *
 * This ensures overrides take precedence, core symbols provide defaults,
 * and extensions add new functionality without conflicts.
 */
const void *hostabi_export_resolve(const char *name)
{
  int i;
  const void *best_override = 0;
  const void *best_extension = 0;
  const void *core_addr = 0;
  int best_priority = -2147483647;
  int best_seq = -2147483647;
  int ext_priority = -2147483647;
  int ext_seq = -2147483647;

  if(name == 0 || name[0] == 0)
    return 0;

  exports_lock();

  /* Find best override and best extension */
  for(i = 0; i < HOSTABI_MODSYM_MAX; i++){
    if(!g_mod_syms[i].used || g_mod_syms[i].addr == 0)
      continue;
    if(strcmp(name, g_mod_syms[i].name) != 0)
      continue;

    if(g_mod_syms[i].kind == HOSTABI_SYMBOL_OVERRIDE){
      if(g_mod_syms[i].priority > best_priority ||
         (g_mod_syms[i].priority == best_priority && g_mod_syms[i].seq > best_seq)){
        best_priority = g_mod_syms[i].priority;
        best_seq = g_mod_syms[i].seq;
        best_override = g_mod_syms[i].addr;
      }
    } else {
      if(g_mod_syms[i].priority > ext_priority ||
         (g_mod_syms[i].priority == ext_priority && g_mod_syms[i].seq > ext_seq)){
        ext_priority = g_mod_syms[i].priority;
        ext_seq = g_mod_syms[i].seq;
        best_extension = g_mod_syms[i].addr;
      }
    }
  }

  /* Check core symbols (search backwards for newest first) */
  for(i = g_core_count - 1; i >= 0; i--){
    if(g_core_syms && g_core_syms[i].addr != 0 && strcmp(name, g_core_syms[i].name) == 0){
      core_addr = g_core_syms[i].addr;
      break;
    }
  }

  /* Return in priority order */
  if(best_override != 0){
    exports_unlock();
    return best_override;
  }

  if(core_addr != 0){
    exports_unlock();
    return core_addr;
  }

  exports_unlock();
  return best_extension;
}
