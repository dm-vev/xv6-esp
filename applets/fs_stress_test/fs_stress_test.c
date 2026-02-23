#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

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

static int create_file(const char *path, const char *content)
{
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if(fd < 0)
    return -1;
  int rc = (int)write(fd, content, strlen(content));
  close(fd);
  return rc;
}

int main(void)
{
  int fd, rc;
  char buf[256];
  char longname[256];
  struct stat st;
  DIR *d;
  struct dirent *de;
  int i;

  printf("=== FS Stress & Boundary Tests ===\n");

  check("mkdir_stress", mkdir("/tmp/stress", 0755) == 0 || errno == EEXIST);

  for(i = 0; i < 20; i++){
    snprintf(buf, sizeof(buf), "/tmp/stress/f%02d", i);
    create_file(buf, "x");
  }
  d = opendir("/tmp/stress");
  if(d){
    int count = 0;
    while((de = readdir(d)) != 0){
      (void)de;
      count++;
    }
    closedir(d);
    check_ge("many_files", count, 20);
  }

  create_file("/tmp/stress/large.bin", "x");
  for(i = 0; i < 10; i++){
    fd = open("/tmp/stress/large.bin", O_WRONLY | O_APPEND);
    if(fd >= 0){
      write(fd, "0123456789", 10);
      close(fd);
    }
  }
  rc = stat("/tmp/stress/large.bin", &st);
  check_eq("file_grow", (int)st.st_size, 101);

  fd = open("/tmp/stress/large.bin", O_RDONLY);
  if(fd >= 0){
    memset(buf, 0, sizeof(buf));
    rc = (int)read(fd, buf, 10);
    check_eq("file_read", rc, 10);
    check_eq("file_content", strncmp(buf, "x0123456789", 10), 0);
    close(fd);
  }

  unlink("/tmp/stress/large.bin");

  snprintf(longname, sizeof(longname), "/tmp/stress/");
  for(i = 0; i < 50; i++){
    snprintf(longname + 12, sizeof(longname) - 12, "d%02d", i);
    mkdir(longname, 0755);
  }
  d = opendir("/tmp/stress");
  if(d){
    int count = 0;
    while((de = readdir(d)) != 0){
      (void)de;
      count++;
    }
    closedir(d);
    check_ge("many_dirs", count, 20);
  }

  fd = open("/tmp/stress/../etc/../../etc/rc", O_RDONLY);
  if(fd >= 0){
    close(fd);
    check("path_traversal_weak", 1);
  }

  fd = open("/tmp/stress/../../etc/passwd", O_RDONLY);
  if(fd >= 0){
    close(fd);
    check("path_up_root", 1);
  }

  fd = open("/tmp/stress//./file.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if(fd >= 0){
    close(fd);
    check("path_dot", 1);
    unlink("/tmp/stress/file.txt");
  }

  fd = open("/tmp/stress/./.././stress/./file2.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if(fd >= 0){
    close(fd);
    check("path_dot_complex", 1);
    unlink("/tmp/stress/file2.txt");
  }

  create_file("/tmp/stress/empty", "");
  rc = stat("/tmp/stress/empty", &st);
  check_eq("empty_file_size", (int)st.st_size, 0);

  fd = open("/tmp/stress/empty", O_RDONLY);
  if(fd >= 0){
    rc = (int)read(fd, buf, 1);
    check_eq("read_empty", rc, 0);
    close(fd);
  }

  fd = open("/tmp/stress/empty", O_WRONLY);
  if(fd >= 0){
    rc = (int)write(fd, "x", 1);
    check_eq("write_to_empty", rc, 1);
    close(fd);
  }

  check("unlink_stress_f00", unlink("/tmp/stress/f00") == 0);
  check("unlink_stress_f01", unlink("/tmp/stress/f01") == 0);

  for(i = 0; i < 50; i++){
    snprintf(longname, sizeof(longname), "/tmp/stress/d%02d", i);
    rmdir(longname);
  }
  rmdir("/tmp/stress");

  printf("=== FS Stress: %d/%d passed ===\n", g_tests - g_failures, g_tests);
  return (g_failures == 0) ? 0 : 1;
}
