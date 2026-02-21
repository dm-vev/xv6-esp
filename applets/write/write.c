#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
  char buf[512];
  unsigned n = 0;
  int i;
  int fd;

  if(argc < 3){
    printf("usage: write path text...\n");
    return 1;
  }

  for(i = 2; i < argc; i++){
    unsigned len = strlen(argv[i]);
    if(n + len + 2 >= sizeof(buf))
      break;
    memcpy(buf + n, argv[i], len);
    n += len;
    if(i + 1 < argc)
      buf[n++] = ' ';
  }
  buf[n++] = '\n';

  fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if(fd < 0){
    printf("write: %s: cannot create\n", argv[1]);
    return 1;
  }
  {
    unsigned off = 0;
    while(off < n){
      int rc = write(fd, buf + off, n - off);
      if(rc <= 0){
        close(fd);
        printf("write: write failed\n");
        return 1;
      }
      off += (unsigned)rc;
    }
  }
  close(fd);
  return 0;
}
