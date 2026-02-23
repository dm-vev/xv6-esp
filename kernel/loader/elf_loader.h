/**
 * @file elf_loader.h
 * @brief ELF loader and dynamic linker interface
 *
 * This header provides ELF loading and dynamic linking functionality for
 * loading shared libraries and executables at runtime. It supports:
 * - Loading ELF modules from memory or flash
 * - Symbol resolution with host symbol table
 * - dlopen/dlsym/dlclose API
 * - Module lifecycle management
 *
 * Capacity limits:
 * - ELFLOADER_MAX_MODULES: Maximum loaded modules (32)
 * - ELFLOADER_MAX_EXPORTS: Maximum exports per module (96)
 * - ELFLOADER_NAME_MAX: Maximum symbol/module name length (32)
 * - ELFLOADER_MAX_HOST_SYMBOLS: Maximum host symbols (4096)
 */
#ifndef XV6_ELF_LOADER_H
#define XV6_ELF_LOADER_H

#include "core/types.h"

/**
 * @brief Maximum number of simultaneously loaded modules
 */
#define ELFLOADER_MAX_MODULES 32

/**
 * @brief Maximum number of exported symbols per module
 */
#define ELFLOADER_MAX_EXPORTS 96

/**
 * @brief Maximum length for symbol/module names
 */
#define ELFLOADER_NAME_MAX 32

/**
 * @brief Maximum number of host symbols in global table
 */
#define ELFLOADER_MAX_HOST_SYMBOLS 4096

/**
 * @brief Host symbol entry
 *
 * Represents a symbol from the host environment (kernel or other modules)
 * that can be resolved by loaded ELF modules.
 */
typedef struct {
  const char *name; /**< Symbol name */
  void *addr;       /**< Symbol address */
} elf_host_symbol_t;

/**
 * @brief Forward declaration of ELF module handle
 */
typedef struct elf_module elf_module_t;

/**
 * @brief dlopen flag: Lazy symbol resolution
 *
 * Resolve undefined symbols on first call to function.
 */
#ifndef RTLD_LAZY
#define RTLD_LAZY 0x00001
#endif

/**
 * @brief dlopen flag: Immediate symbol resolution
 *
 * Resolve all symbols immediately at load time.
 */
#ifndef RTLD_NOW
#define RTLD_NOW 0x00002
#endif

/**
 * @brief dlopen flag: Don't load the module
 *
 * Check if module can be loaded without actually loading.
 */
#ifndef RTLD_NOLOAD
#define RTLD_NOLOAD 0x00004
#endif

/**
 * @brief dlopen flag: Deep binding
 *
 * Search for symbols in the module itself before global symbols.
 */
#ifndef RTLD_DEEPBIND
#define RTLD_DEEPBIND 0x00008
#endif

/**
 * @brief Initialize the ELF loader
 * @return 0 on success, -1 on failure
 *
 * Initializes internal data structures, host symbol table,
 * and module management system.
 */
int elf_loader_init(void);

/**
 * @brief Clear all host symbols
 * @return 0 on success, -1 on failure
 *
 * Removes all symbols from the host symbol table.
 * Used during module unload to clean up module symbols.
 */
int elf_loader_reset_host_symbols(void);

/**
 * @brief Register host symbols
 * @param syms Array of symbol entries
 * @param count Number of symbols
 * @return 0 on success, -1 on failure
 *
 * Adds symbols to the global host symbol table for
 * resolution by loaded modules.
 */
int elf_loader_register_host_symbols(const elf_host_symbol_t *syms, int count);

/**
 * @brief Load ELF from memory
 * @param name Module name (for identification)
 * @param image Pointer to ELF image in memory
 * @param image_size Size of ELF image
 * @param out_mod Output pointer for module handle
 * @return 0 on success, -1 on failure
 *
 * Parses and loads an ELF module from a memory buffer.
 * Performs relocations and symbol resolution.
 */
int elf_module_load_from_bytes(const char *name, const void *image, uint32 image_size, elf_module_t **out_mod);

/**
 * @brief Load ELF from flash
 * @param name Module name
 * @param sector Flash sector number
 * @param sector_count Number of sectors
 * @param out_mod Output module handle
 * @return 0 on success, -1 on failure
 *
 * Loads an ELF module directly from flash memory.
 * More efficient than loading from RAM for large modules.
 */
int elf_module_load_from_flash(const char *name, uint32 sector, uint32 sector_count, elf_module_t **out_mod);

/**
 * @brief Unload an ELF module
 * @param name Module name to unload
 * @return 0 on success, -1 on failure
 *
 * Releases all resources associated with a module,
 * including allocated memory and symbol table entries.
 */
int elf_module_unload(const char *name);

/**
 * @brief Find loaded module by name
 * @param name Module name
 * @return Module handle, or NULL if not found
 */
elf_module_t *elf_module_find(const char *name);

/**
 * @brief Set global symbol visibility
 * @param mod Module handle
 * @param global_visible If true, export symbols globally
 * @return 0 on success, -1 on failure
 *
 * Controls whether symbols from this module are visible
 * to other modules during symbol resolution.
 */
int elf_module_set_global(elf_module_t *mod, int global_visible);

/**
 * @brief Find symbol in module
 * @param mod Module handle
 * @param sym_name Symbol name
 * @return Symbol address, or NULL if not found
 */
void *elf_module_find_symbol(elf_module_t *mod, const char *sym_name);

/**
 * @brief Call function with no arguments
 * @param mod Module handle
 * @param sym_name Function symbol name
 * @param retv Return value pointer
 * @return 0 on success, negative on error
 *
 * Finds and calls a function taking no arguments.
 * Places return value in retv if provided.
 */
int elf_module_call0(elf_module_t *mod, const char *sym_name, int *retv);

/**
 * @brief Call module main function
 * @param mod Module handle
 * @param argc Argument count
 * @param argv Argument vector
 * @param retv Return value pointer
 * @return 0 on success, negative on error
 *
 * Calls the module's main function (or entry point)
 * with standard argc/argv arguments.
 */
int elf_module_call_main(elf_module_t *mod, int argc, char **argv, int *retv);

/**
 * @brief Call module main with environment
 * @param mod Module handle
 * @param argc Argument count
 * @param argv Argument vector
 * @param envp Environment pointer
 * @param retv Return value pointer
 * @return 0 on success, negative on error
 *
 * Like elf_module_call_main but includes environment variables.
 */
int elf_module_call_main_ex(elf_module_t *mod, int argc, char **argv, char **envp, int *retv);

/**
 * @brief Get module information
 * @param mod Module handle
 * @param etype Output: ELF type
 * @param machine Output: machine type
 * @param entry_vaddr Output: entry virtual address
 * @param nsegs Output: number of segments
 * @param nexports Output: number of exported symbols
 * @return 0 on success, -1 on failure
 *
 * Retrieves metadata about a loaded module.
 */
int elf_module_info(elf_module_t *mod, uint16 *etype, uint16 *machine, uint32 *entry_vaddr, int *nsegs, int *nexports);

/**
 * @brief List loaded modules
 * @param names Output array for names
 * @param max_names Maximum names to return
 * @param out_count Actual number of names returned
 *
 * Fills the provided array with names of currently
 * loaded modules.
 */
void elf_module_list(const char **names, int max_names, int *out_count);

/**
 * @brief Translate guest pointer to host pointer
 * @param ptr Guest (xv6) pointer
 * @return Host pointer, or NULL if not translatable
 *
 * Converts pointers from the xv6 virtual address space
 * to the ESP32 physical/host address space. Essential
 * for accessing data structures passed from user programs.
 */
const void *elf_loader_translate_ptr(const void *ptr);

/**
 * @brief Handle host process exit
 * @param status Exit status code
 *
 * Called when an ELF module calls exit(). Cleans up
 * the module and optionally terminates the task.
 */
void elf_loader_host_exit(int status);

/**
 * @brief Cleanup loader state for task
 * @param task_handle FreeRTOS task handle
 *
 * Cleans up any loader resources associated with
 * a specific task that is being deleted.
 */
void elf_loader_task_cleanup_for_handle(void *task_handle);

/**
 * @brief Load a shared library
 * @param file Library name or path
 * @param mode Loading flags (RTLD_*)
 * @return Module handle on success, NULL on failure
 *
 * POSIX dlopen() compatible interface.
 */
void *dlopen(const char *file, int mode);

/**
 * @brief Get symbol address
 * @param handle Module handle from dlopen
 * @param name Symbol name
 * @return Symbol address on success, NULL on failure
 *
 * POSIX dlsym() compatible interface.
 */
void *dlsym(void *handle, const char *name);

/**
 * @brief Unload a shared library
 * @param handle Module handle
 * @return 0 on success, -1 on failure
 *
 * POSIX dlclose() compatible interface.
 */
int dlclose(void *handle);

/**
 * @brief Get last error message
 * @return Error message string, or NULL if no error
 *
 * POSIX dlerror() compatible interface. Returns the
 * error from the last failed dlopen/dlsym/dlclose.
 */
const char *dlerror(void);

#endif
