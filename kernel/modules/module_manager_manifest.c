#include "modules/module_manager_internal.h"

/*
 * Manifest format:
 *   <module_path> [priority] [# optional comment]
 * Blank lines and comments are ignored.
 */
int kmod_autoload_from_manifest(const char *manifest_path, int *loaded_count)
{
  typedef struct {
    char *path;
    int prio;
    int done;
  } manifest_entry_t;

  void *buf = 0;
  uint32 sz = 0;
  char *text;
  char *line;
  manifest_entry_t *entries = 0;
  int entry_cap = 0;
  int entry_count = 0;
  int loaded = 0;
  int i;

  if(loaded_count)
    *loaded_count = 0;

  if(manifest_path == 0 || manifest_path[0] == 0)
    manifest_path = KMOD_DEFAULT_MANIFEST;

  if(xv6fs_read_file_alloc_path(manifest_path, &buf, &sz) != 0 || buf == 0){
    int err = xv6_last_errno();
    if(err == ENOENT || err == 0)
      return 0;
    set_last_error("kmod autoload: manifest read failed");
    return -1;
  }

  text = (char *)malloc((size_t)sz + 1u);
  if(text == 0){
    free(buf);
    set_last_error("kmod autoload: no memory");
    return -1;
  }
  memcpy(text, buf, (size_t)sz);
  text[sz] = 0;
  free(buf);

  entry_cap = 1;
  for(i = 0; i < (int)sz; i++){
    if(text[i] == '\n')
      entry_cap++;
  }
  entries = (manifest_entry_t *)calloc((size_t)entry_cap, sizeof(*entries));
  if(entries == 0){
    free(text);
    set_last_error("kmod autoload: no memory");
    return -1;
  }

  line = text;
  while(*line){
    char *next = strchr(line, '\n');
    char *p;
    char *path_tok;
    int prio = KMOD_PRIORITY_AUTO;

    if(next){
      *next = 0;
      next++;
    }

    p = line;
    while(*p && isspace((unsigned char)*p))
      p++;
    if(*p == '#' || *p == 0){
      line = next ? next : p + strlen(p);
      continue;
    }

    path_tok = p;
    while(*p && !isspace((unsigned char)*p))
      p++;

    if(*p){
      *p++ = 0;
      while(*p && isspace((unsigned char)*p))
        p++;
      if(*p){
        if(parse_line_priority(p, &prio) != 0){
          free(entries);
          free(text);
          set_last_error("kmod autoload: bad priority");
          return -1;
        }
      }
    }

    if(entry_count >= entry_cap){
      free(entries);
      free(text);
      set_last_error("kmod autoload: too many entries");
      return -1;
    }
    entries[entry_count].path = path_tok;
    entries[entry_count].prio = prio;
    entries[entry_count].done = 0;
    entry_count++;

    if(next)
      line = next;
    else
      break;
  }

  while(loaded < entry_count){
    int progress = 0;

    for(i = 0; i < entry_count; i++){
      const char *err;
      if(entries[i].done)
        continue;

      if(kmod_load_with_priority(entries[i].path, entries[i].prio, 0) == 0){
        entries[i].done = 1;
        loaded++;
        progress = 1;
        continue;
      }

      err = kmod_last_error();
      if(err && strstr(err, "unresolved dependency:") != 0)
        continue;

      free(entries);
      free(text);
      return -1;
    }

    if(!progress){
      free(entries);
      free(text);
      set_last_error("kmod autoload: dependency resolution stalled");
      return -1;
    }
  }

  free(entries);
  free(text);
  if(loaded_count)
    *loaded_count = loaded;
  set_last_error("ok");
  return 0;
}
