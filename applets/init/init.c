#include <stdio.h>
#include <unistd.h>

extern int shrt_eval_line(const char *line, int *exit_code);
extern void shrt_reboot(void);

static int run_line(const char *line, int *exit_code)
{
  int rc;
  int code = 0;

  if(line == 0)
    return -1;
  rc = shrt_eval_line(line, &code);
  if(exit_code)
    *exit_code = code;
  return rc;
}

int main(void)
{
  int rc;
  int exit_code = 0;

  rc = run_line("sh -f /etc/rc", &exit_code);
  if(rc != 0 || exit_code != 0){
    printf("init: /etc/rc failed (run=%d, exit=%d), rebooting\n", rc, exit_code);
    fflush(stdout);
    shrt_reboot();
    return 1;
  }

  for(;;){
    printf("init: starting sh\n");
    fflush(stdout);
    if(run_line("sh", &exit_code) != 0){
      printf("init: failed to start sh, rebooting\n");
      fflush(stdout);
      shrt_reboot();
      return 1;
    }
    printf("init: sh exited with status %d, restarting\n", exit_code);
    fflush(stdout);
    usleep(100000);
  }
}
