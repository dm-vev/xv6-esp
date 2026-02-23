#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef READLINK_BUFSZ
#define READLINK_BUFSZ 512
#endif

static void readlink_usage(void)
{
  fprintf(stderr, "usage: readlink [-n] file...\n");
}

int main(int argc, char **argv)
{
  int no_newline = 0;
  int opt;
  int rc = 0;
  int i;
  int files;

  while((opt = getopt(argc, argv, "nh")) != -1){
    switch(opt){
    case 'n':
      no_newline = 1;
      break;
    case 'h':
      readlink_usage();
      return 0;
    default:
      readlink_usage();
      return 1;
    }
  }

  files = argc - optind;
  if(files <= 0){
    readlink_usage();
    return 1;
  }

  for(i = optind; i < argc; i++){
    char buf[READLINK_BUFSZ + 1];
    ssize_t n = readlink(argv[i], buf, READLINK_BUFSZ);
    if(n < 0){
      fprintf(stderr, "readlink: %s: %s\n", argv[i], strerror(errno));
      rc = 1;
      continue;
    }

    buf[n] = 0;
    fputs(buf, stdout);
    if(!no_newline || files > 1)
      fputc('\n', stdout);
  }

  return rc;
}
