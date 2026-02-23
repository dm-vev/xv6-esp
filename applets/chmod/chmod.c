/*
 * Ported and adapted from Embox src/cmds/fs/chmod/chmod.c
 * for xv6-esp applet runtime.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHMOD_MAX_RECURSION 64

static void chmod_usage(void)
{
  fprintf(stderr, "usage: chmod [-R] mode file...\n");
}

static int is_octal_mode(const char *s)
{
  const char *p = s;

  if(s == 0 || *s == 0)
    return 0;
  while(*p){
    if(*p < '0' || *p > '7')
      return 0;
    p++;
  }
  return 1;
}

static int parse_octal_mode(const char *s, mode_t *out_mode)
{
  char *end = 0;
  unsigned long v;

  if(!is_octal_mode(s))
    return 0;

  errno = 0;
  v = strtoul(s, &end, 8);
  if(errno != 0 || end == s || *end != 0){
    fprintf(stderr, "chmod: invalid mode: '%s'\n", s);
    return -1;
  }
  *out_mode = (mode_t)v;
  return 1;
}

static int apply_symbolic_clause(const char *clause, mode_t *mode)
{
  const char *p = clause;
  mode_t who = 0;
  mode_t perm = 0;
  char op;

  while(*p && *p != '+' && *p != '-' && *p != '='){
    switch(*p){
    case 'u':
      who |= 0700;
      break;
    case 'g':
      who |= 0070;
      break;
    case 'o':
      who |= 0007;
      break;
    case 'a':
      who |= 0777;
      break;
    default:
      fprintf(stderr, "chmod: invalid mode: '%s'\n", clause);
      return -1;
    }
    p++;
  }

  if(*p != '+' && *p != '-' && *p != '='){
    fprintf(stderr, "chmod: invalid mode: '%s'\n", clause);
    return -1;
  }
  if(who == 0)
    who = 0777;

  op = *p++;
  if(*p == 0){
    fprintf(stderr, "chmod: invalid mode: '%s'\n", clause);
    return -1;
  }

  while(*p){
    switch(*p){
    case 'r':
      perm |= 0444;
      break;
    case 'w':
      perm |= 0222;
      break;
    case 'x':
      perm |= 0111;
      break;
    default:
      fprintf(stderr, "chmod: invalid mode: '%s'\n", clause);
      return -1;
    }
    p++;
  }

  switch(op){
  case '+':
    *mode |= (perm & who);
    break;
  case '-':
    *mode &= ~(perm & who);
    break;
  case '=':
    *mode &= ~who;
    *mode |= (perm & who);
    break;
  default:
    fprintf(stderr, "chmod: invalid mode: '%s'\n", clause);
    return -1;
  }

  return 0;
}

static int parse_mode(const char *mode_spec, mode_t *mode)
{
  char *tmp = 0;
  char *save = 0;
  char *clause;
  int octal_state;

  octal_state = parse_octal_mode(mode_spec, mode);
  if(octal_state != 0)
    return octal_state < 0 ? -1 : 0;

  tmp = strdup(mode_spec);
  if(tmp == 0){
    fprintf(stderr, "chmod: out of memory\n");
    return -1;
  }

  clause = strtok_r(tmp, ",", &save);
  if(clause == 0){
    free(tmp);
    fprintf(stderr, "chmod: invalid mode: '%s'\n", mode_spec);
    return -1;
  }

  while(clause){
    if(apply_symbolic_clause(clause, mode) != 0){
      free(tmp);
      return -1;
    }
    clause = strtok_r(0, ",", &save);
  }

  free(tmp);
  return 0;
}

static int is_dot_or_dotdot(const char *name)
{
  return (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
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

static int chmod_path(const char *path, const char *mode_spec, int recursive, int depth)
{
  struct stat st;
  mode_t new_mode;
  mode_t old_perm;
  mode_t want_perm;
  int rc = 0;

  if(stat(path, &st) != 0){
    fprintf(stderr, "chmod: %s: %s\n", path, strerror(errno));
    return 1;
  }

  new_mode = st.st_mode;
  if(parse_mode(mode_spec, &new_mode) != 0)
    return 1;
  old_perm = st.st_mode & 07777;
  want_perm = new_mode & 07777;

  if(recursive && S_ISDIR(st.st_mode)){
    DIR *dir = opendir(path);
    struct dirent *ent;

    if(depth >= CHMOD_MAX_RECURSION){
      fprintf(stderr, "chmod: %s: recursion limit reached\n", path);
      rc = 1;
    } else if(dir == 0){
      fprintf(stderr, "chmod: %s: %s\n", path, strerror(errno));
      rc = 1;
    } else {
      while((ent = readdir(dir)) != 0){
        char *child;
        if(is_dot_or_dotdot(ent->d_name))
          continue;
        child = join_path(path, ent->d_name);
        if(child == 0){
          fprintf(stderr, "chmod: out of memory\n");
          rc = 1;
          break;
        }
        rc |= chmod_path(child, mode_spec, recursive, depth + 1);
        free(child);
      }
      closedir(dir);
    }
  }

  if(chmod(path, new_mode) != 0){
    fprintf(stderr, "chmod: %s: %s\n", path, strerror(errno));
    rc = 1;
  } else if(old_perm != want_perm) {
    struct stat st_after;
    if(stat(path, &st_after) != 0 || (st_after.st_mode & 07777) != want_perm){
      fprintf(stderr, "chmod: %s: operation not supported\n", path);
      rc = 1;
    }
  }

  return rc;
}

int main(int argc, char **argv)
{
  int recursive = 0;
  int rc = 0;
  int opt;
  const char *mode_spec;
  int i;

  while((opt = getopt(argc, argv, "Rh")) != -1){
    switch(opt){
    case 'R':
      recursive = 1;
      break;
    case 'h':
      chmod_usage();
      return 0;
    default:
      chmod_usage();
      return 1;
    }
  }

  if(optind + 1 >= argc){
    chmod_usage();
    return 1;
  }

  mode_spec = argv[optind++];
  for(i = optind; i < argc; i++)
    rc |= chmod_path(argv[i], mode_spec, recursive, 0);

  return rc;
}
