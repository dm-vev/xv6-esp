#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static void chgrp_usage(void)
{
  fprintf(stderr, "usage: chgrp group file...\n");
}

static int parse_group(const char *s, gid_t *group)
{
  char *end = 0;
  unsigned long v;

  if(s == 0 || *s == 0)
    return -1;

  errno = 0;
  v = strtoul(s, &end, 10);
  if(errno != 0 || end == s || *end != 0)
    return -1;

  *group = (gid_t)v;
  return 0;
}

int main(int argc, char **argv)
{
  gid_t group = 0;
  int rc = 0;
  int i;

  if(argc < 3){
    chgrp_usage();
    return 1;
  }

  if(parse_group(argv[1], &group) != 0){
    fprintf(stderr, "chgrp: invalid group '%s'\n", argv[1]);
    return 1;
  }

  for(i = 2; i < argc; i++){
    if(chown(argv[i], (uid_t)-1, group) != 0){
      fprintf(stderr, "chgrp: chown(%s, -1, %lu): %s\n",
              argv[i], (unsigned long)group, strerror(errno));
      rc = 1;
    }
  }

  return rc;
}
