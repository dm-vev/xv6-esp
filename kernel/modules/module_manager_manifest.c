#include "modules/module_manager_internal.h"

/*
 * Manifest format:
 *   <module_path> [priority] [# optional comment]
 * Blank lines and comments are ignored.
 */
int kmod_autoload_from_manifest(const char *manifest_path, int *loaded_count)
{
  void *buf = 0;
  uint32 sz = 0;
  char *text;
  char *line;
  int loaded = 0;

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
          free(text);
          set_last_error("kmod autoload: bad priority");
          return -1;
        }
      }
    }

    if(kmod_load_with_priority(path_tok, prio, 0) != 0){
      free(text);
      return -1;
    }
    loaded++;

    if(next)
      line = next;
    else
      break;
  }

  free(text);
  if(loaded_count)
    *loaded_count = loaded;
  set_last_error("ok");
  return 0;
}
