#ifndef XV6_MODULE_MANAGER_H
#define XV6_MODULE_MANAGER_H

#include "core/param.h"
#include "core/types.h"

#define KMOD_MODULE_ABI_VER 1u
#define KMOD_PRIORITY_AUTO (-2147483647 - 1)

#ifndef XV6_KMOD_DEV_MODE_DEFAULT
#define XV6_KMOD_DEV_MODE_DEFAULT 1
#endif

#ifndef XV6_KMOD_ALLOW_UNSIGNED_DEV_DEFAULT
#define XV6_KMOD_ALLOW_UNSIGNED_DEV_DEFAULT 1
#endif

#ifndef XV6_KMOD_ALLOW_FORCE_UNLOAD_DEV_DEFAULT
#define XV6_KMOD_ALLOW_FORCE_UNLOAD_DEV_DEFAULT 1
#endif

#define XV6_MODULE_SYMBOL_EXTENSION 0
#define XV6_MODULE_SYMBOL_OVERRIDE 1

typedef struct {
  int dev_mode;
  int allow_unsigned_dev;
  int allow_force_unload_dev;
} kmod_config_t;

typedef struct {
  const char *name;
  void *addr;
  int kind;
  int priority;
} xv6_module_symbol_t;

typedef struct {
  uint32 abi_ver;
  const char *name;
  int default_priority;
  const xv6_module_symbol_t *symbols;
  int symbol_count;
} xv6_module_desc_t;

typedef const xv6_module_desc_t *(*xv6_module_describe_fn_t)(void);
typedef int (*xv6_module_lifecycle_fn_t)(void);

typedef struct {
  int module_id;
  char name[32];
  char path[MAXPATH];
  int priority;
  int loaded;
  int signed_ok;
  int refcnt;
} kmod_info_t;

int kmod_init(const kmod_config_t *cfg);
int kmod_load(const char *path, int *module_id_out);
int kmod_load_with_priority(const char *path, int priority, int *module_id_out);
int kmod_unload(int module_id, int force);
int kmod_reload(int module_id, int *new_module_id_out);
int kmod_reload_path(const char *path, int priority, int *module_id_out);
int kmod_list(kmod_info_t *out, int cap, int *count_out);
int kmod_autoload_from_manifest(const char *manifest_path, int *loaded_count);
int kmod_verify_signature(const char *path, int *signed_ok);
int kmod_dev_mode(void);
const char *kmod_last_error(void);

#endif
