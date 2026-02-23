#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void ln_usage(void)
{
  fprintf(stderr, "usage: ln [-s] [-f] target [target ...] linkname\n");
}

static int ln_is_dir(const char *path)
{
  struct stat st;
  if(stat(path, &st) != 0)
    return 0;
  return S_ISDIR(st.st_mode) ? 1 : 0;
}

static char *ln_basename_dup(const char *path)
{
  size_t len = strlen(path);
  size_t start;
  char *out;

  while(len > 0 && path[len - 1] == '/')
    len--;
  if(len == 0)
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

static char *ln_join_path(const char *dir, const char *name)
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

static int ln_make_one(const char *target, const char *linkname, int symbolic, int force)
{
  if(force){
    if(unlink(linkname) != 0 && errno != ENOENT){
      fprintf(stderr, "ln: cannot remove '%s': %s\n", linkname, strerror(errno));
      return 1;
    }
  }

  if(symbolic){
    if(symlink(target, linkname) != 0){
      fprintf(stderr, "ln: cannot create symbolic link '%s': %s\n", linkname, strerror(errno));
      return 1;
    }
  } else {
    if(link(target, linkname) != 0){
      fprintf(stderr, "ln: cannot create hard link '%s': %s\n", linkname, strerror(errno));
      return 1;
    }
  }

  return 0;
}

int main(int argc, char **argv)
{
  int symbolic = 0;
  int force = 0;
  int opt;
  int rc = 0;
  int remain;

  while((opt = getopt(argc, argv, "sfh")) != -1){
    switch(opt){
    case 's':
      symbolic = 1;
      break;
    case 'f':
      force = 1;
      break;
    case 'h':
      ln_usage();
      return 0;
    default:
      ln_usage();
      return 1;
    }
  }

  remain = argc - optind;
  if(remain < 2){
    ln_usage();
    return 1;
  }

  if(remain == 2){
    const char *target = argv[optind];
    const char *linkarg = argv[optind + 1];

    if(ln_is_dir(linkarg)){
      char *base = ln_basename_dup(target);
      char *dest = 0;
      int one_rc;

      if(base == 0){
        fprintf(stderr, "ln: out of memory\n");
        return 1;
      }
      dest = ln_join_path(linkarg, base);
      free(base);
      if(dest == 0){
        fprintf(stderr, "ln: out of memory\n");
        return 1;
      }

      one_rc = ln_make_one(target, dest, symbolic, force);
      free(dest);
      return one_rc;
    }

    return ln_make_one(target, linkarg, symbolic, force);
  }

  {
    const char *dest_dir = argv[argc - 1];
    int i;

    if(!ln_is_dir(dest_dir)){
      fprintf(stderr, "ln: target '%s' is not a directory\n", dest_dir);
      return 1;
    }

    for(i = optind; i < argc - 1; i++){
      char *base = ln_basename_dup(argv[i]);
      char *dest = 0;
      if(base == 0){
        fprintf(stderr, "ln: out of memory\n");
        rc = 1;
        continue;
      }
      dest = ln_join_path(dest_dir, base);
      if(dest == 0){
        fprintf(stderr, "ln: out of memory\n");
        free(base);
        rc = 1;
        continue;
      }
      rc |= ln_make_one(argv[i], dest, symbolic, force);
      free(dest);
      free(base);
    }
  }

  return rc;
}
