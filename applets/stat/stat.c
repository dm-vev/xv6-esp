/*
 * Ported and adapted from Embox src/cmds/fs/stat/stat.c
 * for xv6-esp applet runtime.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void stat_usage(void)
{
  printf("usage: stat file...\n");
}

static const char *stat_file_type(const struct stat *st)
{
  if(S_ISCHR(st->st_mode))
    return "character device";
  if(S_ISDIR(st->st_mode))
    return "directory";
  if(S_ISREG(st->st_mode))
    return "regular file";
  if(S_ISBLK(st->st_mode))
    return "block device";
  if(S_ISLNK(st->st_mode))
    return "symbolic link";
  return "unknown";
}

static void stat_print_one(const char *path, const struct stat *st)
{
  printf("  File: %s\n", path);
  printf("\tSize:     %ju\n", (uintmax_t)st->st_size);
  printf("\tBlocks:   %ju\n", (uintmax_t)st->st_blocks);
  printf("\tIO Block: %ju\n", (uintmax_t)st->st_blksize);
  printf("\tType:     %s\n", stat_file_type(st));
  printf("\tDev:      %ju\n", (uintmax_t)st->st_dev);
  printf("\tInode:    %ju\n", (uintmax_t)st->st_ino);
  printf("\tLinks:    %ju\n", (uintmax_t)st->st_nlink);
  printf("\tMode:     %ju\n", (uintmax_t)st->st_mode);
  printf("\tUid:      %ju\n", (uintmax_t)st->st_uid);
  printf("\tGid:      %ju\n", (uintmax_t)st->st_gid);
  printf("\tAccess:   %ju\n", (uintmax_t)st->st_atime);
  printf("\tModify:   %ju\n", (uintmax_t)st->st_mtime);
  printf("\tChange:   %ju\n", (uintmax_t)st->st_ctime);
}

int main(int argc, char **argv)
{
  int rc = 0;
  int i;

  if(argc < 2){
    stat_usage();
    return 1;
  }
  if(argc == 2 && strcmp(argv[1], "-h") == 0){
    stat_usage();
    return 0;
  }

  for(i = 1; i < argc; i++){
    struct stat st;
    if(stat(argv[i], &st) != 0){
      perror(argv[i]);
      rc = 1;
      continue;
    }
    stat_print_one(argv[i], &st);
  }

  return rc;
}
