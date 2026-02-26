#ifndef XV6_MODULE_MANAGER_INTERNAL_H
#define XV6_MODULE_MANAGER_INTERNAL_H

#include "modules/module_manager.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hostabi/hostabi_exports.h"
#include "loader/elf_loader.h"
#include "vfs/vfs.h"

#define KMOD_MAX_TRACKED 16
#define KMOD_ERR_MAX 160
#define KMOD_SIG_MAGIC "XV6SIG1"
#define KMOD_DEFAULT_MANIFEST "/etc/modules.conf"

typedef struct {
  int used;
  int module_id;
  int priority;
  int signed_ok;
  char name[ELFLOADER_NAME_MAX];
  char path[MAXPATH];
  void *handle;
  xv6_module_lifecycle_fn_t fini_fn;
} kmod_slot_t;

extern kmod_slot_t g_slots[KMOD_MAX_TRACKED];
extern kmod_config_t g_cfg;
extern int g_next_module_id;
extern char g_last_error[KMOD_ERR_MAX];
extern SemaphoreHandle_t g_kmod_mu;

void kmod_lock(void);
void kmod_unlock(void);

void set_last_error(const char *fmt, ...);
void copy_cstr(char *dst, int dst_len, const char *src);

int path_to_module_name(const char *path, char *out, int out_len);

int slot_index_by_id_locked(int module_id);
int slot_index_by_path_locked(const char *path);
int slot_index_by_name_locked(const char *name);
int alloc_slot_locked(void);
int alloc_module_id_locked(void);

int parse_signature_for_path(const char *path, int *signed_ok);
int parse_line_priority(const char *tok, int *out_priority);
int register_module_symbols(int module_id, int base_priority, const xv6_module_desc_t *desc);

#endif
