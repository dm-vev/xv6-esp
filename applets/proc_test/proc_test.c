#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

static int g_failures = 0;
static int g_tests = 0;

static void check(const char *name, int ok)
{
  g_tests++;
  if(!ok){
    printf("FAIL %s\n", name);
    g_failures++;
  } else {
    printf("PASS %s\n", name);
  }
}

static void check_eq(const char *name, int a, int b)
{
  g_tests++;
  if(a != b){
    printf("FAIL %s: got %d, expected %d\n", name, a, b);
    g_failures++;
  } else {
    printf("PASS %s\n", name);
  }
}

int main(void)
{
  int pid, status, rc;
  int pipefd[2];

  printf("=== Process Tests ===\n");

  pid = fork();
  check("fork_basic", pid >= 0);

  if(pid == 0){
    sleep(1);
    _exit(42);
  }

  rc = wait(&status);
  check_eq("wait_basic", rc, pid);
  check_eq("wait_status", WIFEXITED(status), 1);
  check_eq("wait_exit", WEXITSTATUS(status), 42);

  rc = wait(&status);
  check_eq("wait_nobody", rc, -1);

  pid = fork();
  if(pid == 0){
    _exit(0);
  }
  wait(&status);
  check("fork_exit_zero", 1);

  pid = fork();
  if(pid == 0){
    _exit(123);
  }
  wait(&status);
  check_eq("fork_exit_nonzero", WEXITSTATUS(status), 123);

  rc = pipe(pipefd);
  check("pipe_basic", rc == 0);

  if(rc == 0){
    pid = fork();
    if(pid == 0){
      close(pipefd[0]);
      write(pipefd[1], "hello", 5);
      close(pipefd[1]);
      _exit(0);
    }
    close(pipefd[1]);
    char buf[16];
    rc = (int)read(pipefd[0], buf, sizeof(buf));
    check_eq("pipe_read", rc, 5);
    buf[5] = '\0';
    check_eq("pipe_content", strcmp(buf, "hello"), 0);
    close(pipefd[0]);
    wait(&status);
  }

  pid = fork();
  if(pid == 0){
    sleep(10);
    _exit(0);
  }
  check("fork_sleep", pid > 0);
  if(pid > 0){
    kill(pid, 9);
    wait(&status);
  }

  printf("=== Proc Test: %d/%d passed ===\n", g_tests - g_failures, g_tests);
  return (g_failures == 0) ? 0 : 1;
}
