#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
  char buf[128];
  int sfd;
  int n;

  if(argc != 2){
    printf("usage: ptyrecv /dev/pts/N\n");
    return 1;
  }

  sfd = open(argv[1], O_RDWR);
  if(sfd < 0){
    printf("ptyrecv: open failed: %s\n", argv[1]);
    return 1;
  }

  n = read(sfd, buf, sizeof(buf));
  if(n < 0){
    close(sfd);
    printf("ptyrecv: read failed\n");
    return 1;
  }

  if(n > 0){
    if(n >= (int)sizeof(buf))
      n = (int)sizeof(buf) - 1;
    buf[n] = 0;
    printf("%s", buf);
  }
  close(sfd);
  return 0;
}
