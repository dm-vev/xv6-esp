/**
 * @file module_manager.h
 * @brief Kernel module manager interface
 *
 * This header defines the kernel module loading and management interface.
 * It provides facilities for:
 * - Loading kernel modules from filesystem
 * - Module lifecycle management (load/unload/reload)
 * - Symbol export to host ABI
 * - Module metadata and information
 * - Signature verification (if enabled)
 *
 * Module descriptors contain:
 * - ABI version for compatibility checking
 * - Module name for identification
 * - Default priority for symbol resolution
 * - Exported symbols with kind and priority
 *
 * Symbol kinds:
 * - EXTENSION: Adds new functionality
 * - OVERRIDE: Replaces existing symbols
 */
#ifndef XV6_MODULE_MANAGER_H
#define XV6_MODULE_MANAGER_H

#include "core/param.h"
#include "core/types.h"

/**
 * @brief Module ABI version
 *
 * Modules must declare this version to ensure compatibility
 * between module and kernel.
 */
#define KMOD_MODULE_ABI_VER 1u

/**
 * @brief Automatic priority assignment
 *
 * Used when no explicit priority is specified. Modules
 * get lowest priority to allow overrides.
 */
#define KMOD_PRIORITY_AUTO (-2147483647 - 1)

/**
 * @brief Default device mode setting
 *
 * If enabled, allows loading modules from dev filesystem.
 */
#ifndef XV6_KMOD_DEV_MODE_DEFAULT
#define XV6_KMOD_DEV_MODE_DEFAULT 1
#endif

/**
 * @brief Default unsigned module policy
 *
 * If enabled, allows loading modules without signature.
 */
#ifndef XV6_KMOD_ALLOW_UNSIGNED_DEV_DEFAULT
#define XV6_KMOD_ALLOW_UNSIGNED_DEV_DEFAULT 1
#endif

/**
 * @brief Default force unload policy
 *
 * If enabled, allows unloading modules even if they're
 * in use.
 */
#ifndef XV6_KMOD_ALLOW_FORCE_UNLOAD_DEV_DEFAULT
#define XV6_KMOD_ALLOW_FORCE_UNLOAD_DEV_DEFAULT 1
#endif

/**
 * @brief Symbol kind: extension
 *
 * Extension symbols add new functionality without
 * overriding existing kernel symbols.
 */
#define XV6_MODULE_SYMBOL_EXTENSION 0

/**
 * @brief Symbol kind: override
 *
 * Override symbols replace existing kernel symbols.
 */
#define XV6_MODULE_SYMBOL_OVERRIDE 1

/**
 * @brief Module manager configuration
 */
typedef struct {
  int dev_mode;                 /**< Allow loading from /dev */
  int allow_unsigned_dev;      /**< Allow unsigned modules in dev mode */
  int allow_force_unload_dev;  /**< Allow force unload in dev mode */
} kmod_config_t;

/**
 * @brief Module symbol entry
 *
 * Represents a single symbol exported by a kernel module.
 */
typedef struct {
  const char *name;    /**< Symbol name */
  void *addr;         /**< Symbol address */
  int kind;           /**< Symbol kind (EXTENSION or OVERRIDE) */
  int priority;       /**< Resolution priority */
} xv6_module_symbol_t;

/**
 * @brief Module descriptor
 *
 * Provides metadata about a kernel module including
 * its name, ABI version, priority, and exported symbols.
 * Modules provide this via a describe function.
 */
typedef struct {
  uint32 abi_ver;                      /**< ABI version (must be KMOD_MODULE_ABI_VER) */
  const char *name;                    /**< Module name */
  int default_priority;                /**< Default priority for symbols */
  const xv6_module_symbol_t *symbols;  /**< Exported symbols array */
  int symbol_count;                    /**< Number of symbols */
} xv6_module_desc_t;

/**
 * @brief Module descriptor function type
 *
 * Modules implement this function to return their descriptor.
 */
typedef const xv6_module_desc_t *(*xv6_module_describe_fn_t)(void);

/**
 * @brief Module lifecycle function type
 *
 * Optional init/exit functions for modules.
 */
typedef int (*xv6_module_lifecycle_fn_t)(void);

/**
 * @brief Module information structure
 */
typedef struct {
  int module_id;      /**< Unique module identifier */
  char name[32];      /**< Module name */
  char path[MAXPATH]; /**< Path from which loaded */
  int priority;      /**< Module priority */
  int loaded;        /**< Currently loaded flag */
  int signed_ok;     /**< Signature verification result */
  int refcnt;       /**< Reference count */
} kmod_info_t;

/**
 * @brief Initialize module manager
 * @param cfg Configuration (or NULL for defaults)
 * @return 0 on success, -1 on failure
 *
 * Sets up the module loading subsystem. Uses default
 * configuration if cfg is NULL.
 */
int kmod_init(const kmod_config_t *cfg);

/**
 * @brief Load a kernel module
 * @param path Path to module file
 * @param module_id_out Output: module ID
 * @return 0 on success, -1 on failure
 *
 * Loads a kernel module from the specified path.
 * Automatically assigns priority.
 */
int kmod_load(const char *path, int *module_id_out);

/**
 * @brief Load with specific priority
 * @param path Path to module
 * @param priority Module priority
 * @param module_id_out Output: module ID
 * @return 0 on success, -1 on failure
 *
 * Loads a module with explicit priority for symbol
 * resolution ordering.
 */
int kmod_load_with_priority(const char *path, int priority, int *module_id_out);

/**
 * @brief Unload a module
 * @param module_id Module to unload
 * @param force Force unload even if in use
 * @return 0 on success, -1 on failure
 *
 * Unloads and frees a previously loaded module.
 * If force is true, ignores reference count.
 */
int kmod_unload(int module_id, int force);

/**
 * @brief Reload a module
 * @param module_id Module to reload
 * @param new_module_id_out Output: new module ID
 * @return 0 on success, -1 on failure
 *
 * Atomically unloads and reloads a module,
 * preserving its ID if possible.
 */
int kmod_reload(int module_id, int *new_module_id_out);

/**
 * @brief Reload from new path
 * @param path New module path
 * @param priority Priority for new module
 * @param module_id_out Output: module ID
 * @return 0 on success, -1 on failure
 *
 * Loads a new module and replaces the specified one.
 */
int kmod_reload_path(const char *path, int priority, int *module_id_out);

/**
 * @brief List loaded modules
 * @param out Output array
 * @param cap Array capacity
 * @param count_out Output: number of modules
 * @return 0 on success, -1 on failure
 *
 * Fills the provided array with information about
 * currently loaded modules.
 */
int kmod_list(kmod_info_t *out, int cap, int *count_out);

/**
 * @brief Autoload modules from manifest
 * @param manifest_path Path to manifest file
 * @param loaded_count_out Output: number loaded
 * @return 0 on success, -1 on failure
 *
 * Reads a manifest file containing module paths
 * and loads them in order.
 */
int kmod_autoload_from_manifest(const char *manifest_path, int *loaded_count);

/**
 * @brief Verify module signature
 * @param path Module path
 * @param signed_ok_out Output: 1 if valid, 0 if invalid
 * @return 0 on success, -1 on error
 *
 * Checks the module's cryptographic signature.
 */
int kmod_verify_signature(const char *path, int *signed_ok);

/**
 * @brief Check if dev mode is enabled
 * @return 1 if dev mode, 0 otherwise
 */
int kmod_dev_mode(void);

/**
 * @brief Get last error message
 * @return Error message string
 *
 * Returns the error from the last failed operation.
 */
const char *kmod_last_error(void);

#endif
