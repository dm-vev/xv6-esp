#include "modules/module_manager_internal.h"

int path_to_module_name(const char *path, char *out, int out_len)
{
  const char *base;
  int len;

  if(path == 0 || out == 0 || out_len <= 1)
    return -1;

  base = strrchr(path, '/');
  base = (base != 0) ? (base + 1) : path;
  if(base[0] == 0)
    return -1;

  copy_cstr(out, out_len, base);
  len = (int)strlen(out);
  if(len > 3 && strcmp(out + len - 3, ".so") == 0)
    out[len - 3] = 0;
  if(out[0] == 0)
    return -1;

  return 0;
}

int parse_signature_for_path(const char *path, int *signed_ok)
{
  char sig_path[MAXPATH + 8];
  int n;
  void *buf = 0;
  uint32 sz = 0;

  if(signed_ok == 0 || path == 0 || path[0] == 0)
    return -1;

  *signed_ok = 0;
  n = snprintf(sig_path, sizeof(sig_path), "%s.sig", path);
  if(n <= 0 || n >= (int)sizeof(sig_path))
    return -1;

  if(xv6fs_read_file_alloc_path(sig_path, &buf, &sz) != 0 || buf == 0)
    return 0;

  if(sz >= (uint32)(sizeof(KMOD_SIG_MAGIC) - 1u) &&
     memcmp(buf, KMOD_SIG_MAGIC, sizeof(KMOD_SIG_MAGIC) - 1u) == 0){
    *signed_ok = 1;
    free(buf);
    return 0;
  }

  free(buf);
  return -1;
}

int parse_line_priority(const char *tok, int *out_priority)
{
  char *endp;
  long v;

  if(tok == 0 || out_priority == 0)
    return -1;

  v = strtol(tok, &endp, 10);
  if(endp == tok)
    return -1;
  while(*endp && isspace((unsigned char)*endp))
    endp++;
  if(*endp && *endp != '#')
    return -1;

  if(v < -2147483647L - 1L || v > 2147483647L)
    return -1;
  *out_priority = (int)v;
  return 0;
}

int register_module_symbols(int module_id, int base_priority, const xv6_module_desc_t *desc)
{
  int i;

  if(desc == 0 || desc->symbol_count <= 0 || desc->symbols == 0)
    return 0;

  for(i = 0; i < desc->symbol_count; i++){
    hostabi_module_symbol_t hs;
    int prio = (desc->symbols[i].priority != 0) ? desc->symbols[i].priority : base_priority;

    hs.name = desc->symbols[i].name;
    hs.addr = desc->symbols[i].addr;
    hs.module_id = module_id;
    hs.priority = prio;
    hs.kind = (desc->symbols[i].kind == XV6_MODULE_SYMBOL_OVERRIDE) ? HOSTABI_SYMBOL_OVERRIDE : HOSTABI_SYMBOL_EXTENSION;

    if(hostabi_export_add_module(&hs, 1) != 0)
      return -1;
  }

  return 0;
}

int collect_module_dependencies(void *handle, const char *const **deps_out, int *count_out)
{
  int *count_ptr;
  const char *const *deps;
  int count;
  int i;

  if(handle == 0 || deps_out == 0 || count_out == 0)
    return -1;

  *deps_out = 0;
  *count_out = 0;

  count_ptr = (int *)dlsym(handle, "xv6_module_depends_count");
  deps = (const char *const *)dlsym(handle, "xv6_module_depends");
  if(count_ptr == 0 && deps == 0)
    return 0;
  if(count_ptr == 0 || deps == 0)
    return -1;

  count = *count_ptr;
  if(count < 0 || count > KMOD_MAX_TRACKED)
    return -1;

  for(i = 0; i < count; i++){
    if(deps[i] == 0 || deps[i][0] == 0)
      return -1;
  }

  *deps_out = deps;
  *count_out = count;
  return 0;
}
