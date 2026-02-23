#include <stdio.h>
#include <string.h>

extern int shrt_eval_line(const char *line, int *exit_code);

static int token_has_whitespace(const char *s)
{
  while(*s){
    if(*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
      return 1;
    s++;
  }
  return 0;
}

int main(int argc, char **argv)
{
  char line[512];
  int i;
  size_t used = 0;
  int exit_code = 0;
  int rc;

  if(argc < 2){
    puts("usage: kmod <list|load|unload|reload|autoload|verify> ...");
    return 2;
  }

  memset(line, 0, sizeof(line));
  used = (size_t)snprintf(line, sizeof(line), "kmod");
  if(used >= sizeof(line))
    return 2;

  for(i = 1; i < argc; i++){
    size_t left;
    int n;

    if(argv[i] == 0 || token_has_whitespace(argv[i])){
      puts("kmod: arguments with whitespace are not supported");
      return 2;
    }
    left = sizeof(line) - used;
    n = snprintf(line + used, left, " %s", argv[i]);
    if(n < 0 || (size_t)n >= left){
      puts("kmod: command line too long");
      return 2;
    }
    used += (size_t)n;
  }

  rc = shrt_eval_line(line, &exit_code);
  if(rc != 0)
    return 1;
  return exit_code;
}
