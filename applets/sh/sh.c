#include <stdio.h>
#include <string.h>

extern int shrt_eval_line(const char *line, int *exit_code);
extern int shrt_run_interactive(void);
extern void shrt_reboot(void);

static int run_script(const char *path)
{
  FILE *f;
  char line[256];

  f = fopen(path, "r");
  if(f == 0)
    return 0;

  while(fgets(line, sizeof(line), f) != 0){
    int exit_code = 0;
    if(shrt_eval_line(line, &exit_code) != 0 || exit_code != 0){
      fclose(f);
      return -1;
    }
  }
  fclose(f);
  return 0;
}

static int run_interactive(void)
{
  return shrt_run_interactive();
}

int main(int argc, char **argv)
{
  if(argc >= 3 && strcmp(argv[1], "-c") == 0){
    int exit_code = 0;
    if(shrt_eval_line(argv[2], &exit_code) != 0)
      return 1;
    return exit_code;
  }

  if(argc >= 3 && strcmp(argv[1], "-f") == 0){
    return run_script(argv[2]) == 0 ? 0 : 1;
  }

  if(run_script("/etc/rc") != 0){
    printf("sh: /etc/rc failed, rebooting\n");
    fflush(stdout);
    shrt_reboot();
    return 1;
  }

  return run_interactive();
}
