#include "loader/elf_loader_internal.h"

/*
 * POSIX-like dynamic loading facade over loader core structures.
 * Handle validation is generation-based to reject stale references.
 */
static uint32 path_hash32(const char *s)
{
  uint32 h = 2166136261u;

  if(s == 0)
    return h;

  while(*s){
    h ^= (uint8)*s++;
    h *= 16777619u;
  }

  return h;
}

static const char *module_name_from_path(const char *path, char *out, int out_len)
{
  const char *base;
  const char *dot;
  int stem_len;
  int max_stem;
  uint32 hash;
  char hsuf[9];

  if(path == 0 || out == 0 || out_len <= 1)
    return 0;

  base = strrchr(path, '/');
  base = (base != 0) ? (base + 1) : path;
  if(base[0] == 0)
    return 0;

  dot = strrchr(base, '.');
  stem_len = (dot && dot > base) ? (int)(dot - base) : (int)strlen(base);
  if(stem_len <= 0)
    return 0;

  max_stem = out_len - 1 - 1 - 8; /* "<stem>_<hash8>" + NUL */
  if(max_stem < 1)
    return 0;
  if(stem_len > max_stem)
    stem_len = max_stem;

  hash = path_hash32(path);
  snprintf(hsuf, sizeof(hsuf), "%08x", (unsigned)hash);

  memcpy(out, base, (unsigned)stem_len);
  out[stem_len] = '_';
  memcpy(out + stem_len + 1, hsuf, 8u);
  out[stem_len + 1 + 8] = 0;
  return out;
}

void *dlopen(const char *file, int mode)
{
  void *image = 0;
  uint32 image_size = 0;
  elf_module_t *mod = 0;
  void *handle = 0;
  char namebuf[ELFLOADER_NAME_MAX];

  (void)mode;

  if(file == 0 || file[0] == 0){
    set_dlerror("dlopen: bad file");
    return 0;
  }

  if(module_name_from_path(file, namebuf, sizeof(namebuf)) == 0){
    set_dlerror("dlopen: bad module name");
    return 0;
  }

  module_lock();
  mod = elf_module_find_locked(namebuf);
  if(mod){
    int idx = module_index_from_ptr_locked((const void *)mod);
    if(idx >= 0)
      handle = module_make_handle_locked(idx);
    if(handle == 0){
      module_unlock();
      set_dlerror("dlopen: handle state");
      return 0;
    }
    mod->open_count++;
    module_unlock();
    set_dlerror(0);
    return handle;
  }
  module_unlock();

  if(xv6fs_read_file_alloc_path(file, &image, &image_size) != 0 || image == 0){
    set_dlerror("dlopen: file not found");
    return 0;
  }

  if(elf_module_load_from_owned_bytes(namebuf, image, image_size, &mod) != 0){
    set_dlerror("dlopen: load failed");
    return 0;
  }

  module_lock();
  if(!module_is_live_locked(mod)){
    module_unlock();
    set_dlerror("dlopen: load race");
    return 0;
  }

  {
    int idx = module_index_from_ptr_locked((const void *)mod);
    if(idx >= 0)
      handle = module_make_handle_locked(idx);
  }

  if(handle == 0){
    module_unlock();
    set_dlerror("dlopen: handle state");
    return 0;
  }

  mod->open_count++;
  module_unlock();

  (void)elf_module_set_global(mod, 1);
  set_dlerror(0);
  return handle;
}

void *dlsym(void *handle, const char *name)
{
  int idx = -1;
  elf_module_t *mod;
  void *sym;

  if(handle == 0 || name == 0 || name[0] == 0){
    set_dlerror("dlsym: bad args");
    return 0;
  }

  module_lock();
  if(module_from_dl_handle_locked(handle, &idx, &mod) != 0 || mod == 0){
    module_unlock();
    set_dlerror("dlsym: bad handle");
    return 0;
  }
  (void)idx;

  sym = elf_module_find_symbol(mod, name);
  module_unlock();

  if(sym == 0){
    set_dlerror("dlsym: symbol not found");
    return 0;
  }

  set_dlerror(0);
  return sym;
}

int dlclose(void *handle)
{
  int idx;
  elf_module_t *mod;

  module_lock();
  if(module_from_dl_handle_locked(handle, &idx, &mod) != 0 || mod == 0){
    module_unlock();
    set_dlerror("dlclose: bad handle");
    return -1;
  }
  if(mod->open_count <= 0){
    module_unlock();
    set_dlerror("dlclose: not open");
    return -1;
  }

  mod->open_count--;
  if(mod->open_count > 0){
    module_unlock();
    set_dlerror(0);
    return 0;
  }

  if(mod->active_calls > 0 || mod->dependent_count > 0 || module_is_active_in_call_ctx(mod)){
    mod->open_count++;
    module_unlock();
    set_dlerror("dlclose: module busy");
    return -1;
  }

  module_unload_index_locked(idx);
  module_unlock();
  set_dlerror(0);
  return 0;
}

const char *dlerror(void)
{
  const char *msg;

  if(!g_dlerror_set)
    return 0;

  msg = g_dlerror_msg;
  g_dlerror_set = 0;
  return msg;
}

int elf_loader_handle_refstate(void *handle, int *open_count_out, int *active_calls_out, int *dependent_count_out,
                               int *in_call_ctx_out)
{
  int idx = -1;
  elf_module_t *mod = 0;
  int in_ctx = 0;

  if(open_count_out)
    *open_count_out = 0;
  if(active_calls_out)
    *active_calls_out = 0;
  if(dependent_count_out)
    *dependent_count_out = 0;
  if(in_call_ctx_out)
    *in_call_ctx_out = 0;

  module_lock();
  if(module_from_dl_handle_locked(handle, &idx, &mod) != 0 || mod == 0){
    module_unlock();
    return -1;
  }
  (void)idx;

  if(open_count_out)
    *open_count_out = mod->open_count;
  if(active_calls_out)
    *active_calls_out = mod->active_calls;
  if(dependent_count_out)
    *dependent_count_out = mod->dependent_count;
  in_ctx = module_is_active_in_call_ctx(mod);
  if(in_call_ctx_out)
    *in_call_ctx_out = in_ctx;
  module_unlock();
  return 0;
}
