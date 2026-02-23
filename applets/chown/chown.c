/*
 * Ported and adapted from Embox src/cmds/fs/chown/chown.c
 * for xv6-esp applet runtime.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void chown_usage(void)
{
  fprintf(stderr, "usage: chown owner[:group] file...\n");
}

static int parse_id(const char *s, unsigned long *out)
{
  char *end = 0;
  unsigned long v;

  if(s == 0 || *s == 0)
    return -1;
  errno = 0;
  v = strtoul(s, &end, 10);
  if(errno != 0 || end == s || *end != 0)
    return -1;
  *out = v;
  return 0;
}

static int parse_owner_group(const char *spec, uid_t *owner, gid_t *group, int *has_group)
{
  const char *sep = strchr(spec, ':');
  unsigned long owner_v = 0;
  unsigned long group_v = 0;
  char owner_buf[32];

  if(sep == 0){
    if(parse_id(spec, &owner_v) != 0)
      return -1;
    *owner = (uid_t)owner_v;
    *group = (gid_t)0;
    *has_group = 0;
    return 0;
  }

  if((size_t)(sep - spec) >= sizeof(owner_buf))
    return -1;
  memcpy(owner_buf, spec, (size_t)(sep - spec));
  owner_buf[sep - spec] = 0;

  if(parse_id(owner_buf, &owner_v) != 0 || parse_id(sep + 1, &group_v) != 0)
    return -1;

  *owner = (uid_t)owner_v;
  *group = (gid_t)group_v;
  *has_group = 1;
  return 0;
}

static int chown_one(const char *path, uid_t owner, gid_t group, int has_group)
{
  gid_t gid = group;

  if(!has_group){
    struct stat st;
    if(stat(path, &st) != 0){
      fprintf(stderr, "chown: stat(%s): %s\n", path, strerror(errno));
      return 1;
    }
    gid = st.st_gid;
  }

  if(chown(path, owner, gid) != 0){
    fprintf(stderr, "chown: chown(%s, %lu, %lu): %s\n",
            path, (unsigned long)owner, (unsigned long)gid, strerror(errno));
    return 1;
  }

  return 0;
}

int main(int argc, char **argv)
{
  uid_t owner = 0;
  gid_t group = 0;
  int has_group = 0;
  int rc = 0;
  int i;

  if(argc < 3){
    chown_usage();
    return 1;
  }

  if(parse_owner_group(argv[1], &owner, &group, &has_group) != 0){
    fprintf(stderr, "chown: invalid owner/group spec '%s'\n", argv[1]);
    return 1;
  }

  for(i = 2; i < argc; i++)
    rc |= chown_one(argv[i], owner, group, has_group);

  return rc;
}
