#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
  char out[256];
  unsigned len;
  int i;
  int cut = -1;

  if(argc != 2){
    printf("usage: dirname path\n");
    return 1;
  }

  len = strlen(argv[1]);
  if(len == 0){
    printf(".\n");
    return 0;
  }
  if(len >= sizeof(out))
    len = sizeof(out) - 1;
  memcpy(out, argv[1], len);
  out[len] = 0;

  for(i = (int)len - 1; i >= 0; i--){
    if(out[i] == '/'){
      cut = i;
      break;
    }
  }

  if(cut < 0){
    printf(".\n");
    return 0;
  }
  if(cut == 0){
    printf("/\n");
    return 0;
  }
  out[cut] = 0;
  printf("%s\n", out);
  return 0;
}
