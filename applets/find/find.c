#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FIND_MAX_RECURSION 128

extern int lstat(const char *path, struct stat *st);

typedef struct {
  const char *name_pattern;
  int type_filter;
  int min_depth;
  int max_depth;
} find_opts_t;

static void find_usage(void)
{
  fprintf(stderr, "usage: find [path ...] [-name pattern] [-type f|d] [-mindepth n] [-maxdepth n] [-print]\n");
}

static int parse_nonneg_int(const char *s, int *out)
{
  char *end = 0;
  long v;

  if(s == 0 || *s == 0)
    return -1;
  errno = 0;
  v = strtol(s, &end, 10);
  if(errno != 0 || end == s || *end != 0 || v < 0)
    return -1;
  *out = (int)v;
  return 0;
}

static int patmatch(const char *pattern, const char *text)
{
  if(*pattern == 0)
    return *text == 0;

  if(*pattern == '*'){
    while(pattern[1] == '*')
      pattern++;
    if(pattern[1] == 0)
      return 1;
    while(*text){
      if(patmatch(pattern + 1, text))
        return 1;
      text++;
    }
    return patmatch(pattern + 1, text);
  }

  if(*pattern == '?'){
    if(*text == 0)
      return 0;
    return patmatch(pattern + 1, text + 1);
  }

  if(*pattern != *text)
    return 0;
  return patmatch(pattern + 1, text + 1);
}

static char *path_basename_dup(const char *path)
{
  size_t len = strlen(path);
  size_t start;
  char *out;

  while(len > 1 && path[len - 1] == '/')
    len--;
  if(len == 1 && path[0] == '/')
    return strdup("/");

  start = len;
  while(start > 0 && path[start - 1] != '/')
    start--;

  out = (char *)malloc(len - start + 1);
  if(out == 0)
    return 0;
  memcpy(out, path + start, len - start);
  out[len - start] = 0;
  return out;
}

static int match_entry(const char *path, const struct stat *st, int depth, const find_opts_t *opts)
{
  char *base_name = 0;
  int name_match = 1;

  if(depth < opts->min_depth)
    return 0;
  if(opts->max_depth >= 0 && depth > opts->max_depth)
    return 0;

  if(opts->type_filter == 'f' && !S_ISREG(st->st_mode))
    return 0;
  if(opts->type_filter == 'd' && !S_ISDIR(st->st_mode))
    return 0;

  if(opts->name_pattern){
    base_name = path_basename_dup(path);
    if(base_name == 0)
      return 0;
    name_match = patmatch(opts->name_pattern, base_name);
    free(base_name);
  }
  if(!name_match)
    return 0;

  return 1;
}

static char *join_path(const char *dir, const char *name)
{
  size_t dir_len = strlen(dir);
  size_t name_len = strlen(name);
  int need_slash = (dir_len > 0 && dir[dir_len - 1] != '/');
  size_t out_len = dir_len + (size_t)need_slash + name_len + 1;
  char *out = (char *)malloc(out_len);

  if(out == 0)
    return 0;
  if(need_slash)
    snprintf(out, out_len, "%s/%s", dir, name);
  else
    snprintf(out, out_len, "%s%s", dir, name);
  return out;
}

static int is_dot_or_dotdot(const char *name)
{
  return (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
}

static int find_walk(const char *path, int depth, const find_opts_t *opts)
{
  struct stat st;
  int rc = 0;

  if(depth > FIND_MAX_RECURSION){
    fprintf(stderr, "find: %s: recursion limit reached\n", path);
    return 1;
  }

  if(lstat(path, &st) != 0){
    fprintf(stderr, "find: %s: %s\n", path, strerror(errno));
    return 1;
  }

  if(match_entry(path, &st, depth, opts))
    puts(path);

  if(!S_ISDIR(st.st_mode))
    return rc;
  if(opts->max_depth >= 0 && depth >= opts->max_depth)
    return rc;

  {
    DIR *dir = opendir(path);
    struct dirent *ent;

    if(dir == 0){
      fprintf(stderr, "find: %s: %s\n", path, strerror(errno));
      return 1;
    }

    while((ent = readdir(dir)) != 0){
      char *child;
      if(is_dot_or_dotdot(ent->d_name))
        continue;

      child = join_path(path, ent->d_name);
      if(child == 0){
        fprintf(stderr, "find: out of memory\n");
        rc = 1;
        break;
      }
      rc |= find_walk(child, depth + 1, opts);
      free(child);
    }

    closedir(dir);
  }

  return rc;
}

int main(int argc, char **argv)
{
  find_opts_t opts;
  const char **paths = 0;
  int npaths = 0;
  int cap_paths = 0;
  int i = 1;
  int rc = 0;

  memset(&opts, 0, sizeof(opts));
  opts.min_depth = 0;
  opts.max_depth = -1;

  if(argc > 1 && strcmp(argv[1], "-h") == 0){
    find_usage();
    return 0;
  }

  while(i < argc && argv[i][0] != '-'){
    if(npaths >= cap_paths){
      int ncap = cap_paths ? cap_paths * 2 : 8;
      const char **np = (const char **)realloc(paths, (size_t)ncap * sizeof(*paths));

      if(np == 0){
        fprintf(stderr, "find: out of memory\n");
        free(paths);
        return 1;
      }
      paths = np;
      cap_paths = ncap;
    }
    paths[npaths++] = argv[i];
    i++;
  }

  if(npaths == 0){
    paths = (const char **)malloc(sizeof(*paths));
    if(paths == 0){
      fprintf(stderr, "find: out of memory\n");
      return 1;
    }
    paths[npaths++] = ".";
    cap_paths = 1;
  }

  while(i < argc){
    if(strcmp(argv[i], "-name") == 0){
      if(i + 1 >= argc){
        find_usage();
        free(paths);
        return 1;
      }
      opts.name_pattern = argv[i + 1];
      i += 2;
      continue;
    }
    if(strcmp(argv[i], "-type") == 0){
      if(i + 1 >= argc || (argv[i + 1][0] != 'f' && argv[i + 1][0] != 'd') || argv[i + 1][1] != 0){
        find_usage();
        free(paths);
        return 1;
      }
      opts.type_filter = argv[i + 1][0];
      i += 2;
      continue;
    }
    if(strcmp(argv[i], "-maxdepth") == 0){
      if(i + 1 >= argc || parse_nonneg_int(argv[i + 1], &opts.max_depth) != 0){
        find_usage();
        free(paths);
        return 1;
      }
      i += 2;
      continue;
    }
    if(strcmp(argv[i], "-mindepth") == 0){
      if(i + 1 >= argc || parse_nonneg_int(argv[i + 1], &opts.min_depth) != 0){
        find_usage();
        free(paths);
        return 1;
      }
      i += 2;
      continue;
    }
    if(strcmp(argv[i], "-print") == 0){
      i++;
      continue;
    }

    fprintf(stderr, "find: unsupported expression '%s'\n", argv[i]);
    free(paths);
    return 1;
  }

  for(i = 0; i < npaths; i++)
    rc |= find_walk(paths[i], 0, &opts);

  free(paths);
  return rc;
}
