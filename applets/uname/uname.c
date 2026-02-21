#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MFLAG 0x01
#define NFLAG 0x02
#define RFLAG 0x04
#define SFLAG 0x08
#define VFLAG 0x10

static void usage(void)
{
  (void)fprintf(stderr, "usage: uname [-amnrsv]\n");
  exit(1);
}

int main(int argc, char *argv[])
{
  unsigned int flags = 0;
  int ch;
  const char *prefix = "";
  const char *sysname = "xv6-esp";
  const char *nodename = "esp32s3";
  const char *release = "1.0";
  const char *version = __DATE__ " " __TIME__;
  const char *machine = "xtensa-esp32s3";

  while((ch = getopt(argc, argv, "amnrsv")) != -1){
    switch(ch){
    case 'a':
      flags |= (MFLAG | NFLAG | RFLAG | SFLAG | VFLAG);
      break;
    case 'm':
      flags |= MFLAG;
      break;
    case 'n':
      flags |= NFLAG;
      break;
    case 'r':
      flags |= RFLAG;
      break;
    case 's':
      flags |= SFLAG;
      break;
    case 'v':
      flags |= VFLAG;
      break;
    default:
      usage();
    }
  }

  argc -= optind;
  if(argc != 0)
    usage();

  if(flags == 0)
    flags |= SFLAG;

  if(flags & SFLAG){
    (void)printf("%s%s", prefix, sysname);
    prefix = " ";
  }
  if(flags & NFLAG){
    (void)printf("%s%s", prefix, nodename);
    prefix = " ";
  }
  if(flags & RFLAG){
    (void)printf("%s%s", prefix, release);
    prefix = " ";
  }
  if(flags & VFLAG){
    (void)printf("%s%s", prefix, version);
    prefix = " ";
  }
  if(flags & MFLAG){
    (void)printf("%s%s", prefix, machine);
  }
  (void)printf("\n");
  return 0;
}
