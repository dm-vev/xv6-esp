#ifndef XV6_MODULE_H
#define XV6_MODULE_H

#include <stdint.h>

#define KMOD_MODULE_ABI_VER 1u

#define XV6_MODULE_SYMBOL_EXTENSION 0
#define XV6_MODULE_SYMBOL_OVERRIDE 1

typedef struct {
  const char *name;
  void *addr;
  int kind;
  int priority;
} xv6_module_symbol_t;

typedef struct {
  uint32_t abi_ver;
  const char *name;
  int default_priority;
  const xv6_module_symbol_t *symbols;
  int symbol_count;
} xv6_module_desc_t;

typedef const xv6_module_desc_t *(*xv6_module_describe_fn_t)(void);
typedef int (*xv6_module_lifecycle_fn_t)(void);

/*
 * Optional module dependency metadata:
 *   const char *xv6_module_depends[] = { "core", "/lib/modules/net.so" };
 *   int xv6_module_depends_count = sizeof(xv6_module_depends) / sizeof(xv6_module_depends[0]);
 */

#endif
