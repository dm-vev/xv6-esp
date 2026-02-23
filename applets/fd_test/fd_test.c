#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static void check_ge(const char *name, int a, int b)
{
  g_tests++;
  if(a < b){
    printf("FAIL %s: got %d, expected >= %d\n", name, a, b);
    g_failures++;
  } else {
    printf("PASS %s\n", name);
  }
}

int main(void)
{
  int fd, fds[32];
  int i, rc;
  char buf[16];

  printf("=== FD Tests ===\n");

  fd = open("/dev/null", O_RDWR);
  check("open_dev_null", fd >= 0);
  if(fd >= 0){
    check_eq("read_dev_null", (int)read(fd, buf, 1), 0);
    check_eq("write_dev_null", (int)write(fd, "x", 1), 1);
    close(fd);
  }

  fd = open("/dev/zero", O_RDWR);
  check("open_dev_zero", fd >= 0);
  if(fd >= 0){
    memset(buf, 0xFF, sizeof(buf));
    rc = (int)read(fd, buf, sizeof(buf));
    check_ge("read_dev_zero", rc, 0);
    check_eq("zero_is_zero", buf[0], 0);
    close(fd);
  }

  fd = open("/dev/full", O_WRONLY);
  check("open_dev_full", fd >= 0);
  if(fd >= 0){
    rc = (int)write(fd, "x", 1);
    check("write_dev_full", rc < 0 && errno == EIO);
    close(fd);
  }

  for(i = 0; i < 20; i++){
    fds[i] = open("/dev/null", O_RDWR);
    if(fds[i] < 0)
      break;
  }
  check_eq("fd_many_open", i, 20);

  for(i = 0; i < 20; i++){
    if(fds[i] >= 0)
      close(fds[i]);
  }

  check("close_negative", close(-1) < 0 && errno == EBADF);

  fd = open("/tmp/fd_test_file", O_RDWR | O_CREAT | O_TRUNC, 0644);
  if(fd >= 0){
    check("write_then_lseek", (int)write(fd, "hello", 5) == 5);
    check_eq("lseek_set", (int)lseek(fd, 0, SEEK_SET), 0);
    rc = (int)read(fd, buf, sizeof(buf));
    check_eq("read_after_lseek", rc, 5);
    check_eq("lseek_cur", (int)lseek(fd, 2, SEEK_CUR), 7);
    check_eq("lseek_end", (int)lseek(fd, 0, SEEK_END), 5);
    check_eq("lseek_invalid", (int)lseek(fd, 0, 999), -1);
    close(fd);
    unlink("/tmp/fd_test_file");
  }

  fd = open("/dev/null", O_RDWR);
  check("fcntl_getfl", fcntl(fd, F_GETFL) >= 0);
  if(fd >= 0)
    close(fd);

  fd = open("/tmp/fd_test_file2", O_RDWR | O_CREAT | O_TRUNC, 0644);
  if(fd >= 0){
    int fl = fcntl(fd, F_GETFL);
    check("fcntl_setfl", fcntl(fd, F_SETFL, fl | O_APPEND) == 0);
    write(fd, "x", 1);
    lseek(fd, 0, SEEK_SET);
    write(fd, "y", 1);
    lseek(fd, 0, SEEK_SET);
    rc = (int)read(fd, buf, 2);
    buf[2] = '\0';
    check_eq("fcntl_append", rc, 2);
    close(fd);
    unlink("/tmp/fd_test_file2");
  }

  printf("=== FD Test: %d/%d passed ===\n", g_tests - g_failures, g_tests);
  return (g_failures == 0) ? 0 : 1;
}
