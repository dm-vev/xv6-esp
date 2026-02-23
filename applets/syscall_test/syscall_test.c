#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>

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

static void check_ne(const char *name, int a, int b)
{
  g_tests++;
  if(a == b){
    printf("FAIL %s: got %d, expected != %d\n", name, a, b);
    g_failures++;
  } else {
    printf("PASS %s\n", name);
  }
}

static void check_gt(const char *name, int a, int b)
{
  g_tests++;
  if(a <= b){
    printf("FAIL %s: got %d, expected > %d\n", name, a, b);
    g_failures++;
  } else {
    printf("PASS %s\n", name);
  }
}

int main(void)
{
  int pid;
  int rc;
  char buf[64];

  printf("=== Syscall Boundary Tests ===\n");

  pid = getpid();
  check_gt("getpid_positive", pid, 0);

  pid = getppid();
  check("getppid_nonnegative", pid >= 0);

  rc = chdir("/tmp");
  check_eq("chdir_tmp", rc, 0);

  rc = chdir("/");
  check_eq("chdir_root", rc, 0);

  getcwd(buf, sizeof(buf));
  check_eq("getcwd_root", strcmp(buf, "/"), 0);

  rc = chdir("/tmp");
  if(rc == 0){
    getcwd(buf, sizeof(buf));
    check_eq("getcwd_tmp", strcmp(buf, "/tmp"), 0);
  }

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  check_gt("clock_realtime_positive", (int)ts.tv_sec, 1700000000);

  clock_gettime(CLOCK_MONOTONIC, &ts);
  check_gt("clock_monotonic_positive", (int)ts.tv_sec, 0);

  struct timeval tv;
  gettimeofday(&tv, 0);
  check_gt("gettimeofday_positive", (int)tv.tv_sec, 1700000000);

  rc = sleep(0);
  check_eq("sleep_zero", rc, 0);

  rc = sleep(1);
  check_eq("sleep_one", rc, 0);

  unsigned int ticks = 0;
  (void)ticks;

  rc = getuid();
  check_eq("getuid_zero", rc, 0);

  rc = geteuid();
  check_eq("geteuid_zero", rc, 0);

  rc = getgid();
  check_eq("getgid_zero", rc, 0);

  rc = getegid();
  check_eq("getegid_zero", rc, 0);

  printf("=== Syscall Test: %d/%d passed ===\n", g_tests - g_failures, g_tests);
  return (g_failures == 0) ? 0 : 1;
}
