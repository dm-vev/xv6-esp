#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

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

static void check_str(const char *name, const char *a, const char *b)
{
  g_tests++;
  if(strcmp(a, b) != 0){
    printf("FAIL %s: got '%s', expected '%s'\n", name, a, b);
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
  struct stat st;
  DIR *d;
  struct dirent *de;

  printf("=== FS Stress Tests ===\n");

  check("stat_nonexistent", stat("/no/such/file", &st) < 0 && errno == ENOENT);

  check("mkdir_basic", mkdir("/tmp/fs_test", 0755) == 0 || errno == EEXIST);
  check("mkdir_nested", mkdir("/tmp/fs_test/nested", 0755) == 0 || errno == EEXIST);
  check("mkdir_nested2", mkdir("/tmp/fs_test/nested/deep", 0755) == 0 || errno == EEXIST);

  rc = create_file("/tmp/fs_test/file1.txt", "hello world");
  check_eq("create_file", rc, 11);

  rc = stat("/tmp/fs_test/file1.txt", &st);
  check_eq("stat_file", rc, 0);
  check_eq("stat_size", (int)st.st_size, 11);

  fd = open("/tmp/fs_test/file1.txt", O_RDONLY);
  check_ge("open_file", fd, 0);
  if(fd >= 0){
    rc = (int)read(fd, buf, sizeof(buf) - 1);
    check_ge("read_file", rc, 0);
    if(rc > 0){
      buf[rc] = '\0';
      check_str("read_content", buf, "hello world");
    }
    close(fd);
  }

  fd = open("/tmp/fs_test/file1.txt", O_WRONLY | O_APPEND);
  if(fd >= 0){
    rc = (int)write(fd, " extra", 6);
    check_eq("append_file", rc, 6);
    close(fd);
  }

  rc = stat("/tmp/fs_test/file1.txt", &st);
  check_eq("stat_appended", (int)st.st_size, 17);

  fd = open("/tmp/fs_test/file1.txt", O_RDONLY);
  if(fd >= 0){
    rc = (int)read(fd, buf, sizeof(buf) - 1);
    if(rc > 0){
      buf[rc] = '\0';
      check_str("verify_append", buf, "hello world extra");
    }
    close(fd);
  }

  check("unlink_file", unlink("/tmp/fs_test/file1.txt") == 0);
  check("unlink_nonexistent", unlink("/tmp/fs_test/no_such") < 0 && errno == ENOENT);

  check("rename_basic", rename("/tmp/fs_test/nested", "/tmp/fs_test/nested_renamed") == 0);
  check("rename_nonexistent", rename("/tmp/fs_test/no_such", "/tmp/fs_test/other") < 0 && errno == ENOENT);

  create_file("/tmp/fs_test/renamed_file", "test content");
  check("rename_file", rename("/tmp/fs_test/renamed_file", "/tmp/fs_test/new_name") == 0);
  check("stat_renamed", stat("/tmp/fs_test/new_name", &st) == 0);
  check("stat_old_failed", stat("/tmp/fs_test/renamed_file", &st) < 0 && errno == ENOENT);

  create_file("/tmp/fs_test/trunc_test", "long content here");
  fd = open("/tmp/fs_test/trunc_test", O_WRONLY | O_TRUNC);
  check_ge("trunc_open", fd, 0);
  if(fd >= 0){
    rc = fstat(fd, &st);
    check_eq("trunc_size", (int)st.st_size, 0);
    close(fd);
  }

  d = opendir("/tmp/fs_test");
  check("opendir_basic", d != 0);
  if(d){
    int count = 0;
    while((de = readdir(d)) != 0){
      (void)de;
      count++;
    }
    closedir(d);
    check_ge("readdir_count", count, 2);
  }

  check("rmdir_nested", rmdir("/tmp/fs_test/nested_renamed/deep") == 0);
  check("rmdir_nested2", rmdir("/tmp/fs_test/nested_renamed") == 0);
  check("rmdir_nonexistent", rmdir("/tmp/fs_test/no_such") < 0 && errno == ENOENT);
  check("rmdir_notempty", rmdir("/tmp/fs_test") < 0 && errno == ENOTEMPTY);
  check("cleanup_new_name", unlink("/tmp/fs_test/new_name") == 0);
  check("cleanup_trunc_test", unlink("/tmp/fs_test/trunc_test") == 0);

  check("rmdir_final", rmdir("/tmp/fs_test") == 0);

  printf("=== FS Stress: %d/%d passed ===\n", g_tests - g_failures, g_tests);
  return (g_failures == 0) ? 0 : 1;
}
