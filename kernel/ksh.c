#include "ksh.h"

#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/reent.h>
#include <dirent.h>
#include <signal.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "elf_loader.h"
#include "esp_flash_disk.h"
#include "hal.h"
#include "param.h"
#include "xv6fs_ro.h"

#define KSH_MAX_JOBS 32
#define KSH_MAX_ARGS 32
#define KSH_MAX_STAGES 8
#define KSH_MAX_ENV 16
#define KSH_ENV_KEY 24
#define KSH_ENV_VAL 128
#define KSH_BG_STACK 8192

enum {
  JOB_REASON_NONE = 0,
  JOB_REASON_EXIT,
  JOB_REASON_TIMEOUT,
  JOB_REASON_KILLED,
};

typedef struct {
  int used;
  int id;
  int done;
  int exit_code;
  int reason;
  int is_pipe;
  int max_heap_kb;
  uint32 max_runtime_ms;
  uint32 started_ms;
  TaskHandle_t task;
  void *task_ctx;
  char cmd[96];
} ksh_job_t;

typedef struct {
  int slot;
  int argc;
  char **argv;
  int in_fd;
  int out_fd;
  int err_fd;
  int max_heap_kb;
  char cwd[MAXPATH];
} ksh_job_task_t;

typedef struct {
  int in_fd;
  int out_fd;
  int err_fd;
} ksh_io_t;

typedef struct {
  int used;
  char key[KSH_ENV_KEY];
  char val[KSH_ENV_VAL];
} ksh_env_t;

static ksh_job_t g_jobs[KSH_MAX_JOBS];
static int g_next_job_id = 1;
static uint32 g_ulimit_ms = 0;
static int g_ulimit_heap_kb = 0;
static volatile int g_ksh_started = 0;
static SemaphoreHandle_t g_jobs_lock;
static SemaphoreHandle_t g_loader_lock;
static ksh_env_t g_env[KSH_MAX_ENV];
static char g_loaded_module[MAXPATH];
static int g_loaded_module_valid = 0;

static int dispatch_command(int argc, char **argv, int run_bg);

#define XV6_KSTAT_T_DIR 1
#define XV6_KSTAT_T_FILE 2
#define XV6_KSTAT_T_DEVICE 3

static int k_ticks(void)
{
  return (int)hal_ticks();
}

static int k_free_heap(void)
{
  return (int)hal_free_heap_bytes();
}

static int k_puts(const char *s)
{
  s = (const char *)elf_loader_translate_ptr(s);
  if(s == 0)
    return -1;
  while(*s)
    hal_console_putc(*s++);
  hal_console_putc('\r');
  hal_console_putc('\n');
  return 0;
}

typedef struct ksh_stream ksh_stream_t;
static ksh_stream_t *k_stream_from_file(FILE *f);
static size_t k_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
static int k_host_vfprintf(FILE *stream, const char *fmt, va_list ap);
static void k_exit(int status);
static int k_open(const char *path, int flags, ...);
static int k_read(int fd, void *buf, size_t size);
static int k_write(int fd, const void *buf, size_t size);
static int k_close(int fd);
static off_t k_lseek(int fd, off_t offset, int whence);

static int k_host_printf(const char *fmt, ...)
{
  va_list ap;
  int n;
  fmt = (const char *)elf_loader_translate_ptr(fmt);
  if(fmt == 0)
    return -1;
  va_start(ap, fmt);
  n = vprintf(fmt, ap);
  va_end(ap);
  return n;
}

static int k_host_puts(const char *s)
{
  s = (const char *)elf_loader_translate_ptr(s);
  if(s == 0)
    return -1;
  return puts(s);
}

static int k_host_fprintf(FILE *stream, const char *fmt, ...)
{
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = k_host_vfprintf(stream, fmt, ap);
  va_end(ap);
  return n;
}

static unsigned int k_host_strlen(const char *s)
{
  s = (const char *)elf_loader_translate_ptr(s);
  if(s == 0)
    return 0;
  return (unsigned int)strlen(s);
}

static int k_host_strcmp(const char *a, const char *b)
{
  a = (const char *)elf_loader_translate_ptr(a);
  b = (const char *)elf_loader_translate_ptr(b);
  if(a == 0 || b == 0)
    return (a == b) ? 0 : (a ? 1 : -1);
  return strcmp(a, b);
}

static int k_host_vfprintf(FILE *stream, const char *fmt, va_list ap)
{
  int n;
  char buf[256];
  ksh_stream_t *ks;

  fmt = (const char *)elf_loader_translate_ptr(fmt);
  if(fmt == 0)
    return -1;
  ks = k_stream_from_file(stream);
  if(ks){
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if(n > 0){
      size_t out_n = (size_t)n;
      if(out_n >= sizeof(buf))
        out_n = sizeof(buf) - 1u;
      (void)k_fwrite(buf, 1, out_n, stream);
    }
    return n;
  }
  return vfprintf(stream, fmt, ap);
}

#define KSH_STREAM_MAGIC 0x4b534831u
#define KSH_MAX_STREAMS 24

struct ksh_stream {
  uint32 magic;
  int fd;
  int eof;
  int err;
  int has_ungot;
  int ungot;
};

static ksh_stream_t g_streams[KSH_MAX_STREAMS];

static ksh_stream_t *k_stream_from_file(FILE *f)
{
  uintptr_t p = (uintptr_t)f;
  uintptr_t start = (uintptr_t)&g_streams[0];
  uintptr_t end = (uintptr_t)(&g_streams[KSH_MAX_STREAMS - 1] + 1);
  ksh_stream_t *s;

  if(f == 0 || p < start || p >= end)
    return 0;
  s = (ksh_stream_t *)f;
  if(s->magic != KSH_STREAM_MAGIC)
    return 0;
  return s;
}

static FILE *k_stream_alloc(int fd)
{
  int i;
  for(i = 0; i < KSH_MAX_STREAMS; i++){
    if(g_streams[i].magic == 0){
      memset(&g_streams[i], 0, sizeof(g_streams[i]));
      g_streams[i].magic = KSH_STREAM_MAGIC;
      g_streams[i].fd = fd;
      return (FILE *)&g_streams[i];
    }
  }
  return 0;
}

static void k_stream_release(ksh_stream_t *s)
{
  if(s == 0)
    return;
  memset(s, 0, sizeof(*s));
}

static int k_stdio_mode_to_flags(const char *mode)
{
  int flags;
  int plus = 0;
  const char *p;

  if(mode == 0 || mode[0] == 0){
    errno = EINVAL;
    return -1;
  }
  for(p = mode; *p; p++){
    if(*p == '+')
      plus = 1;
  }

  switch(mode[0]){
  case 'r':
    flags = plus ? O_RDWR : O_RDONLY;
    break;
  case 'w':
    flags = plus ? O_RDWR : O_WRONLY;
    flags |= O_CREAT | O_TRUNC;
    break;
  case 'a':
    flags = plus ? O_RDWR : O_WRONLY;
    flags |= O_CREAT | O_APPEND;
    break;
  default:
    errno = EINVAL;
    return -1;
  }
  return flags;
}

static FILE *k_fopen(const char *path, const char *mode)
{
  int flags;
  int fd;
  FILE *f;

  path = (const char *)elf_loader_translate_ptr(path);
  mode = (const char *)elf_loader_translate_ptr(mode);
  if(path == 0 || mode == 0){
    errno = EINVAL;
    return 0;
  }
  flags = k_stdio_mode_to_flags(mode);
  if(flags < 0)
    return 0;
  fd = k_open(path, flags, 0666);
  if(fd < 0)
    return 0;
  f = k_stream_alloc(fd);
  if(f == 0){
    (void)k_close(fd);
    errno = ENOMEM;
  }
  return f;
}

static FILE *k_freopen(const char *path, const char *mode, FILE *stream)
{
  ksh_stream_t *s = k_stream_from_file(stream);
  FILE *f;

  path = (const char *)elf_loader_translate_ptr(path);
  mode = (const char *)elf_loader_translate_ptr(mode);
  if(path == 0 || mode == 0){
    errno = EINVAL;
    return 0;
  }
  if(s){
    (void)k_close(s->fd);
    k_stream_release(s);
  }
  f = k_fopen(path, mode);
  if(f && s && f != stream){
    ksh_stream_t *old = (ksh_stream_t *)stream;
    *old = *(ksh_stream_t *)f;
    k_stream_release((ksh_stream_t *)f);
    return stream;
  }
  return f;
}

static int k_fclose(FILE *stream)
{
  ksh_stream_t *s = k_stream_from_file(stream);
  if(s){
    int rc = k_close(s->fd);
    k_stream_release(s);
    return rc;
  }
  return fclose(stream);
}

static int k_fgetc(FILE *stream)
{
  ksh_stream_t *s = k_stream_from_file(stream);
  unsigned char ch;
  int rc;

  if(!s)
    return fgetc(stream);
  if(s->has_ungot){
    s->has_ungot = 0;
    return s->ungot & 0xff;
  }
  rc = k_read(s->fd, &ch, 1);
  if(rc == 1)
    return (int)ch;
  if(rc == 0)
    s->eof = 1;
  else
    s->err = 1;
  return EOF;
}

static int k_getc(FILE *stream)
{
  return k_fgetc(stream);
}

static int k_getchar(void)
{
  unsigned char ch;
  int rc = k_read(0, &ch, 1);
  if(rc == 1)
    return (int)ch;
  return EOF;
}

static char *k_fgets(char *s, int n, FILE *stream)
{
  int i;
  if(s == 0 || n <= 0){
    errno = EINVAL;
    return 0;
  }
  for(i = 0; i < n - 1; i++){
    int c = k_fgetc(stream);
    if(c == EOF)
      break;
    s[i] = (char)c;
    if(c == '\n'){
      i++;
      break;
    }
  }
  if(i == 0)
    return 0;
  s[i] = 0;
  return s;
}

static size_t k_fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
  ksh_stream_t *s = k_stream_from_file(stream);
  size_t want;
  int rc;

  if(!s)
    return fread(ptr, size, nmemb, stream);
  if(size == 0 || nmemb == 0)
    return 0;
  want = size * nmemb;
  rc = k_read(s->fd, ptr, want);
  if(rc <= 0){
    if(rc == 0)
      s->eof = 1;
    else
      s->err = 1;
    return 0;
  }
  return (size_t)rc / size;
}

static size_t k_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
  ksh_stream_t *s = k_stream_from_file(stream);
  size_t want;
  int rc;
  ptr = elf_loader_translate_ptr(ptr);
  if(ptr == 0)
    return 0;
  if(!s)
    return fwrite(ptr, size, nmemb, stream);
  if(size == 0 || nmemb == 0)
    return 0;
  want = size * nmemb;
  rc = k_write(s->fd, ptr, want);
  if(rc < 0){
    s->err = 1;
    return 0;
  }
  return (size_t)rc / size;
}

static int k_fputc(int c, FILE *stream)
{
  unsigned char ch = (unsigned char)c;
  return (k_fwrite(&ch, 1, 1, stream) == 1) ? c : EOF;
}

static int k_putc(int c, FILE *stream)
{
  return k_fputc(c, stream);
}

static int k_fputs(const char *s, FILE *stream)
{
  size_t n;
  s = (const char *)elf_loader_translate_ptr(s);
  if(s == 0){
    errno = EINVAL;
    return EOF;
  }
  n = strlen(s);
  return (k_fwrite(s, 1, n, stream) == n) ? 0 : EOF;
}

static int k_fflush(FILE *stream)
{
  ksh_stream_t *s = k_stream_from_file(stream);
  if(s)
    return 0;
  return fflush(stream);
}

static void k_clearerr(FILE *stream)
{
  ksh_stream_t *s = k_stream_from_file(stream);
  if(s){
    s->err = 0;
    s->eof = 0;
    return;
  }
  clearerr(stream);
}

static int k_feof(FILE *stream)
{
  ksh_stream_t *s = k_stream_from_file(stream);
  if(s)
    return s->eof;
  return feof(stream);
}

static int k_ferror(FILE *stream)
{
  ksh_stream_t *s = k_stream_from_file(stream);
  if(s)
    return s->err;
  return ferror(stream);
}

static int k_fseek(FILE *stream, long offset, int whence)
{
  ksh_stream_t *s = k_stream_from_file(stream);
  if(s){
    if(k_lseek(s->fd, (off_t)offset, whence) < 0){
      s->err = 1;
      return -1;
    }
    s->eof = 0;
    return 0;
  }
  return fseek(stream, offset, whence);
}

static long k_ftell(FILE *stream)
{
  ksh_stream_t *s = k_stream_from_file(stream);
  if(s){
    off_t rc = k_lseek(s->fd, 0, SEEK_CUR);
    return (rc < 0) ? -1L : (long)rc;
  }
  return ftell(stream);
}

static void k_rewind(FILE *stream)
{
  (void)k_fseek(stream, 0, SEEK_SET);
  k_clearerr(stream);
}

static int k_ungetc(int c, FILE *stream)
{
  ksh_stream_t *s = k_stream_from_file(stream);
  if(s){
    if(s->has_ungot)
      return EOF;
    s->has_ungot = 1;
    s->ungot = c & 0xff;
    s->eof = 0;
    return c;
  }
  return ungetc(c, stream);
}

static int k_fileno(FILE *stream)
{
  ksh_stream_t *s = k_stream_from_file(stream);
  if(s)
    return s->fd;
  return fileno(stream);
}

static void k_setbuf(FILE *stream, char *buf)
{
  if(k_stream_from_file(stream))
    return;
  setbuf(stream, buf);
}

static int k_setvbuf(FILE *stream, char *buf, int mode, size_t size)
{
  if(k_stream_from_file(stream))
    return 0;
  return setvbuf(stream, buf, mode, size);
}

static void *k_host_memcpy(void *dst, const void *src, unsigned int n)
{
  src = elf_loader_translate_ptr(src);
  if(dst == 0 || src == 0)
    return dst;
  return memcpy(dst, src, n);
}

static int k_bcmp(const void *a, const void *b, size_t n)
{
  return memcmp(a, b, n);
}

static void k_bcopy(const void *src, void *dst, size_t n)
{
  if(src == 0 || dst == 0)
    return;
  (void)memmove(dst, src, n);
}

static void k_bzero(void *dst, size_t n)
{
  if(dst == 0)
    return;
  (void)memset(dst, 0, n);
}

static int k_map_open_flags(int flags)
{
  int xv6_flags = 0;
  switch(flags & O_ACCMODE){
  case O_WRONLY:
    xv6_flags |= XV6_O_WRONLY;
    break;
  case O_RDWR:
    xv6_flags |= XV6_O_RDWR;
    break;
  default:
    xv6_flags |= XV6_O_RDONLY;
    break;
  }
  if(flags & O_CREAT)
    xv6_flags |= XV6_O_CREAT;
  if(flags & O_TRUNC)
    xv6_flags |= XV6_O_TRUNC;
  if(flags & O_APPEND)
    xv6_flags |= XV6_O_APPEND;
  return xv6_flags;
}

static mode_t k_mode_from_xv6_type(uint16 type)
{
  if(type == XV6_KSTAT_T_DIR)
    return (mode_t)(S_IFDIR | 0777);
  if(type == XV6_KSTAT_T_DEVICE)
    return (mode_t)(S_IFCHR | 0666);
  return (mode_t)(S_IFREG | 0666);
}

static int k_fill_host_stat(const xv6_kstat_t *kst, struct stat *st)
{
  if(kst == 0 || st == 0)
    return -1;
  memset(st, 0, sizeof(*st));
  st->st_ino = (ino_t)kst->ino;
  st->st_nlink = (nlink_t)(kst->nlink ? kst->nlink : 1);
  st->st_mode = k_mode_from_xv6_type(kst->type);
  st->st_size = (off_t)kst->size;
  return 0;
}

static int k_open(const char *path, int flags, ...)
{
  int fd;
  mode_t mode = 0;
  va_list ap;
  path = (const char *)elf_loader_translate_ptr(path);
  if(path == 0){
    errno = EINVAL;
    return -1;
  }
  va_start(ap, flags);
  mode = (mode_t)va_arg(ap, int);
  va_end(ap);
  (void)mode;
  fd = xv6_open(path, k_map_open_flags(flags));
  if(fd < 0)
    errno = ENOENT;
  return fd;
}

static int k_creat(const char *path, mode_t mode)
{
  return k_open(path, O_CREAT | O_TRUNC | O_WRONLY, mode);
}

static int k_read(int fd, void *buf, size_t size)
{
  int rc = xv6_read(fd, buf, (uint32)size);
  if(rc < 0)
    errno = EIO;
  return rc;
}

static int k_write(int fd, const void *buf, size_t size)
{
  int rc = xv6_write(fd, buf, (uint32)size);
  if(rc < 0)
    errno = EIO;
  return rc;
}

static int k_close(int fd)
{
  if(fd >= 0 && fd <= 2)
    return 0;
  if(xv6_close(fd) != 0){
    errno = EBADF;
    return -1;
  }
  return 0;
}

static int k_dup(int fd)
{
  int rc = xv6_dup(fd);
  if(rc < 0){
    errno = EBADF;
    return -1;
  }
  return rc;
}

static int k_dup2(int oldfd, int newfd)
{
  if(oldfd == newfd)
    return newfd;
  if(newfd >= 0)
    (void)k_close(newfd);
  return k_dup(oldfd);
}

static off_t k_lseek(int fd, off_t offset, int whence)
{
  int rc = xv6_lseek(fd, (int)offset, whence);
  if(rc < 0){
    errno = EINVAL;
    return (off_t)-1;
  }
  return (off_t)rc;
}

static int k_fstat(int fd, struct stat *st)
{
  xv6_kstat_t kst;
  if(st == 0){
    errno = EINVAL;
    return -1;
  }
  if(xv6_fstat(fd, &kst) != 0){
    errno = EBADF;
    return -1;
  }
  return k_fill_host_stat(&kst, st);
}

static int k_stat(const char *path, struct stat *st)
{
  xv6_kstat_t kst;
  path = (const char *)elf_loader_translate_ptr(path);
  if(path == 0 || st == 0){
    errno = EINVAL;
    return -1;
  }
  if(xv6_stat_path(path, &kst) != 0){
    errno = ENOENT;
    return -1;
  }
  return k_fill_host_stat(&kst, st);
}

static int k_lstat(const char *path, struct stat *st)
{
  return k_stat(path, st);
}

static int k_access(const char *path, int mode)
{
  path = (const char *)elf_loader_translate_ptr(path);
  if(path == 0){
    errno = EINVAL;
    return -1;
  }
  if(xv6_access(path, mode) != 0){
    errno = ENOENT;
    return -1;
  }
  return 0;
}

static int k_chmod(const char *path, mode_t mode)
{
  path = (const char *)elf_loader_translate_ptr(path);
  if(path == 0){
    errno = EINVAL;
    return -1;
  }
  if(xv6_chmod(path, (int)mode) != 0){
    errno = ENOENT;
    return -1;
  }
  return 0;
}

static int k_mkdir(const char *path, mode_t mode)
{
  (void)mode;
  path = (const char *)elf_loader_translate_ptr(path);
  if(path == 0){
    errno = EINVAL;
    return -1;
  }
  if(xv6fs_mkdir_path(path) != 0){
    errno = EIO;
    return -1;
  }
  return 0;
}

static int k_unlink(const char *path)
{
  path = (const char *)elf_loader_translate_ptr(path);
  if(path == 0){
    errno = EINVAL;
    return -1;
  }
  if(xv6fs_unlink_path(path) != 0){
    errno = ENOENT;
    return -1;
  }
  return 0;
}

static int k_rmdir(const char *path)
{
  return k_unlink(path);
}

static int k_chdir(const char *path)
{
  path = (const char *)elf_loader_translate_ptr(path);
  if(path == 0){
    errno = EINVAL;
    return -1;
  }
  if(xv6_chdir(path) != 0){
    errno = ENOENT;
    return -1;
  }
  return 0;
}

static char *k_getcwd(char *buf, size_t size)
{
  if(buf == 0 || size == 0){
    errno = EINVAL;
    return 0;
  }
  if(xv6_getcwd(buf, (int)size) != 0){
    errno = ERANGE;
    return 0;
  }
  return buf;
}

static int k_isatty(int fd)
{
  return (fd >= 0 && fd <= 2) ? 1 : 0;
}

static int k_utimes(const char *path, const struct timeval times[2])
{
  (void)times;
  path = (const char *)elf_loader_translate_ptr(path);
  if(path == 0){
    errno = EINVAL;
    return -1;
  }
  return 0;
}

static int k_lutimes(const char *path, const struct timeval times[2])
{
  return k_utimes(path, times);
}

static mode_t g_umask = 0;

static mode_t k_umask(mode_t mask)
{
  mode_t old = g_umask;
  g_umask = mask;
  return old;
}

static int k_fsync(int fd)
{
  (void)fd;
  return 0;
}

static int k_fdatasync(int fd)
{
  (void)fd;
  return 0;
}

static void k_sync(void)
{
}

static int k_ftruncate(int fd, off_t length)
{
  (void)fd;
  (void)length;
  return 0;
}

static int k_truncate(const char *path, off_t length)
{
  path = (const char *)elf_loader_translate_ptr(path);
  (void)length;
  if(path == 0){
    errno = EINVAL;
    return -1;
  }
  return 0;
}

static int k_link(const char *oldpath, const char *newpath)
{
  oldpath = (const char *)elf_loader_translate_ptr(oldpath);
  newpath = (const char *)elf_loader_translate_ptr(newpath);
  if(oldpath == 0 || newpath == 0){
    errno = EINVAL;
    return -1;
  }
  errno = ENOSYS;
  return -1;
}

static int k_rename(const char *oldpath, const char *newpath)
{
  oldpath = (const char *)elf_loader_translate_ptr(oldpath);
  newpath = (const char *)elf_loader_translate_ptr(newpath);
  if(oldpath == 0 || newpath == 0){
    errno = EINVAL;
    return -1;
  }
  errno = ENOSYS;
  return -1;
}

static int k_symlink(const char *target, const char *linkpath)
{
  target = (const char *)elf_loader_translate_ptr(target);
  linkpath = (const char *)elf_loader_translate_ptr(linkpath);
  if(target == 0 || linkpath == 0){
    errno = EINVAL;
    return -1;
  }
  errno = ENOSYS;
  return -1;
}

static int k_readlink(const char *path, char *buf, size_t bufsz)
{
  path = (const char *)elf_loader_translate_ptr(path);
  (void)buf;
  (void)bufsz;
  if(path == 0){
    errno = EINVAL;
    return -1;
  }
  errno = ENOSYS;
  return -1;
}

static int k_mknod(const char *path, mode_t mode, dev_t dev)
{
  path = (const char *)elf_loader_translate_ptr(path);
  (void)mode;
  (void)dev;
  if(path == 0){
    errno = EINVAL;
    return -1;
  }
  errno = ENOSYS;
  return -1;
}

static int k_mkfifo(const char *path, mode_t mode)
{
  path = (const char *)elf_loader_translate_ptr(path);
  (void)mode;
  if(path == 0){
    errno = EINVAL;
    return -1;
  }
  errno = ENOSYS;
  return -1;
}

static void (*k_signal(int sig, void (*handler)(int)))(int)
{
  (void)sig;
  (void)handler;
  errno = ENOSYS;
  return SIG_ERR;
}

static int k_sigaction(int sig, const struct sigaction *act, struct sigaction *oldact)
{
  (void)sig;
  (void)act;
  (void)oldact;
  return 0;
}

static int k_sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
  (void)how;
  (void)set;
  if(oldset)
    (void)sigemptyset(oldset);
  return 0;
}

static int k_sigemptyset(sigset_t *set)
{
  if(set == 0){
    errno = EINVAL;
    return -1;
  }
  memset(set, 0, sizeof(*set));
  return 0;
}

static int k_sigfillset(sigset_t *set)
{
  if(set == 0){
    errno = EINVAL;
    return -1;
  }
  memset(set, 0xff, sizeof(*set));
  return 0;
}

static int k_sigaddset(sigset_t *set, int signo)
{
  (void)signo;
  if(set == 0){
    errno = EINVAL;
    return -1;
  }
  return 0;
}

static int k_sigdelset(sigset_t *set, int signo)
{
  (void)signo;
  if(set == 0){
    errno = EINVAL;
    return -1;
  }
  return 0;
}

static int k_sigismember(const sigset_t *set, int signo)
{
  (void)set;
  (void)signo;
  return 0;
}

static int k_raise(int sig)
{
  (void)sig;
  return 0;
}

static unsigned int k_sleep(unsigned int sec)
{
  usleep(sec * 1000000U);
  return 0;
}

static int k_fchmod(int fd, mode_t mode)
{
  (void)fd;
  (void)mode;
  return 0;
}

static int k_chown(const char *path, uid_t owner, gid_t group)
{
  path = (const char *)elf_loader_translate_ptr(path);
  (void)owner;
  (void)group;
  if(path == 0){
    errno = EINVAL;
    return -1;
  }
  return 0;
}

static int k_lchown(const char *path, uid_t owner, gid_t group)
{
  return k_chown(path, owner, group);
}

static int k_fchown(int fd, uid_t owner, gid_t group)
{
  (void)fd;
  (void)owner;
  (void)group;
  return 0;
}

static int k_optind = 1;
static int k_opterr = 1;
static int k_optopt;
static int k_optreset;
static char *k_optarg;
static int k_getopt_pos = 1;

static void k_getopt_reset_state(void)
{
  k_optarg = 0;
  k_getopt_pos = 1;
  k_optreset = 0;
}

static int k_getopt(int argc, char *const argv_in[], const char *optstring_in)
{
  char *const *argv;
  const char *optstring;
  const char *arg;
  const char *optp;
  int c;
  int need_arg;

  argv = (char *const *)elf_loader_translate_ptr(argv_in);
  optstring = (const char *)elf_loader_translate_ptr(optstring_in);
  if(argv == 0 || optstring == 0 || argc <= 0)
    return -1;

  if(k_optreset || k_optind <= 0){
    k_optind = 1;
    k_getopt_reset_state();
  }

  if(k_optind >= argc)
    return -1;

  arg = (const char *)elf_loader_translate_ptr(argv[k_optind]);
  if(arg == 0)
    return -1;

  if(arg[0] != '-' || arg[1] == '\0')
    return -1;

  if(arg[0] == '-' && arg[1] == '-' && arg[2] == '\0'){
    k_optind++;
    k_getopt_reset_state();
    return -1;
  }

  c = (unsigned char)arg[k_getopt_pos];
  if(c == 0){
    k_optind++;
    k_getopt_pos = 1;
    return k_getopt(argc, argv_in, optstring_in);
  }

  optp = strchr(optstring, c);
  if(optp == 0){
    k_optopt = c;
    if(arg[++k_getopt_pos] == '\0'){
      k_optind++;
      k_getopt_pos = 1;
    }
    return '?';
  }

  need_arg = (optp[1] == ':') ? 1 : 0;
  if(!need_arg){
    k_optarg = 0;
    if(arg[++k_getopt_pos] == '\0'){
      k_optind++;
      k_getopt_pos = 1;
    }
    return c;
  }

  if(arg[k_getopt_pos + 1] != '\0'){
    k_optarg = (char *)elf_loader_translate_ptr(arg + k_getopt_pos + 1);
    k_optind++;
    k_getopt_pos = 1;
    return c;
  }

  if((k_optind + 1) < argc){
    const char *next = (const char *)elf_loader_translate_ptr(argv[k_optind + 1]);
    k_optarg = (char *)next;
    k_optind += 2;
    k_getopt_pos = 1;
    return c;
  }

  k_optopt = c;
  if(optstring[0] == ':')
    return ':';
  return '?';
}

static _off_t k__lseek_r(struct _reent *r, int fd, _off_t off, int whence)
{
  (void)r;
  return (_off_t)k_lseek(fd, (off_t)off, whence);
}

static _ssize_t k__read_r(struct _reent *r, int fd, void *buf, size_t cnt)
{
  (void)r;
  return (_ssize_t)k_read(fd, buf, cnt);
}

static _ssize_t k__write_r(struct _reent *r, int fd, const void *buf, size_t cnt)
{
  (void)r;
  return (_ssize_t)k_write(fd, buf, cnt);
}

static int k__close_r(struct _reent *r, int fd)
{
  (void)r;
  return k_close(fd);
}

static int k__open_r(struct _reent *r, const char *path, int flags, int mode)
{
  (void)r;
  return k_open(path, flags, mode);
}

static int k__fstat_r(struct _reent *r, int fd, struct stat *st)
{
  (void)r;
  return k_fstat(fd, st);
}

static int k__stat_r(struct _reent *r, const char *path, struct stat *st)
{
  (void)r;
  return k_stat(path, st);
}

static int k__isatty_r(struct _reent *r, int fd)
{
  (void)r;
  return k_isatty(fd);
}

static int k__unlink_r(struct _reent *r, const char *path)
{
  (void)r;
  return k_unlink(path);
}

static int k__kill_r(struct _reent *r, int pid, int sig)
{
  (void)r;
  (void)pid;
  (void)sig;
  errno = ENOSYS;
  return -1;
}

static int k__getpid_r(struct _reent *r)
{
  (void)r;
  return 1;
}

static void k__exit(int code)
{
  k_exit(code);
}

static caddr_t k__sbrk_r(struct _reent *r, ptrdiff_t incr)
{
  static char *arena;
  static size_t used;
  static size_t cap;
  size_t old;

  (void)r;
  if(arena == 0){
    cap = 64 * 1024;
    arena = (char *)malloc(cap);
    used = 0;
  }
  if(arena == 0 || incr < 0 || used + (size_t)incr > cap){
    errno = ENOMEM;
    return (caddr_t)-1;
  }
  old = used;
  used += (size_t)incr;
  return (caddr_t)(arena + old);
}

static void k_exit(int status)
{
  elf_loader_host_exit(status);
}

static void k_abort(void)
{
  elf_loader_host_exit(134);
}

extern int ksh_register_libc_host_symbols(void);
extern struct _reent *__getreent(void);

static int k_fs_readdir_path(const char *path, int index, char *name_out, int name_out_len, uint16 *type_out,
                             uint32 *size_out)
{
  return xv6fs_list_path(path, index, name_out, name_out_len, type_out, size_out);
}

static void tty_putc(int c)
{
  hal_console_putc(c);
}

static void tty_puts(const char *s)
{
  while(*s)
    tty_putc(*s++);
}

static void putc_console(int c)
{
  char ch = (char)c;
  int rc = xv6_write(1, &ch, 1);
  if(rc < 0)
    tty_putc(c);
}

static void puts_console(const char *s)
{
  int n;
  if(s == 0)
    return;
  n = (int)strlen(s);
  if(n <= 0)
    return;
  if(xv6_stdio_is_default_out()){
    (void)fwrite(s, 1, (size_t)n, stdout);
    return;
  }
  if(xv6_write(1, s, (uint32)n) == n)
    return;
  while(*s)
    tty_putc(*s++);
}

static void eputs_console(const char *s)
{
  int n;
  if(s == 0)
    return;
  n = (int)strlen(s);
  if(n <= 0)
    return;
  if(xv6_stdio_is_default_out()){
    (void)fwrite(s, 1, (size_t)n, stderr);
    return;
  }
  if(xv6_write(2, s, (uint32)n) == n)
    return;
  while(*s)
    hal_console_putc(*s++);
}

static void puts_line(const char *s)
{
  puts_console(s);
  puts_console("\r\n");
}

static void eputs_line(const char *s)
{
  eputs_console(s);
  eputs_console("\r\n");
}

static void k_vprintf_fd(int fd, const char *fmt, va_list ap)
{
  char buf[256];
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  if(n < 0)
    return;
  if(n >= (int)sizeof(buf))
    n = (int)sizeof(buf) - 1;
  if(fd == 2){
    if(xv6_write(2, buf, (uint32)n) != n)
      return;
  } else {
    if(xv6_write(1, buf, (uint32)n) != n)
      return;
  }
}

static void k_printf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  k_vprintf_fd(1, fmt, ap);
  va_end(ap);
}

static void k_eprintf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  k_vprintf_fd(2, fmt, ap);
  va_end(ap);
}

static void print_u32(uint32 v)
{
  char tmp[11];
  int i = 0;

  if(v == 0){
    putc_console('0');
    return;
  }

  while(v > 0 && i < (int)(sizeof(tmp) - 1)){
    tmp[i++] = (char)('0' + (v % 10));
    v /= 10;
  }
  while(i > 0)
    putc_console(tmp[--i]);
}

static int parse_u32_dec(const char *s, uint32 *out)
{
  uint32 v = 0;
  if(s == 0 || *s == 0 || out == 0)
    return -1;
  while(*s){
    if(*s < '0' || *s > '9')
      return -1;
    v = v * 10 + (uint32)(*s - '0');
    s++;
  }
  *out = v;
  return 0;
}

static int env_find_slot(const char *key)
{
  int i;
  for(i = 0; i < KSH_MAX_ENV; i++){
    if(g_env[i].used && strcmp(g_env[i].key, key) == 0)
      return i;
  }
  return -1;
}

static const char *env_get(const char *key)
{
  int i = env_find_slot(key);
  if(i >= 0)
    return g_env[i].val;
  return 0;
}

static int env_set(const char *key, const char *val)
{
  int i = env_find_slot(key);
  int free_i = -1;
  int j;

  if(key == 0 || val == 0 || key[0] == 0 || strlen(key) >= KSH_ENV_KEY || strlen(val) >= KSH_ENV_VAL)
    return -1;
  for(j = 0; key[j]; j++){
    if(!(key[j] == '_' || (key[j] >= '0' && key[j] <= '9') || (key[j] >= 'A' && key[j] <= 'Z') ||
         (key[j] >= 'a' && key[j] <= 'z')))
      return -1;
  }

  if(i < 0){
    for(j = 0; j < KSH_MAX_ENV; j++){
      if(!g_env[j].used){
        free_i = j;
        break;
      }
    }
    if(free_i < 0)
      return -1;
    i = free_i;
    memset(&g_env[i], 0, sizeof(g_env[i]));
    g_env[i].used = 1;
    strcpy(g_env[i].key, key);
  }
  strcpy(g_env[i].val, val);
  return 0;
}

static void env_unset(const char *key)
{
  int i = env_find_slot(key);
  if(i >= 0)
    memset(&g_env[i], 0, sizeof(g_env[i]));
}

static void env_sync_pwd(void)
{
  char cwd[MAXPATH];
  if(xv6_getcwd(cwd, sizeof(cwd)) == 0)
    (void)env_set("PWD", cwd);
}

static void env_init_defaults(void)
{
  memset(g_env, 0, sizeof(g_env));
  (void)env_set("PATH", "/bin:/usr/bin:.");
  (void)env_set("HOME", "/");
  env_sync_pwd();
}

static const char *job_reason_str(int reason)
{
  switch(reason){
  case JOB_REASON_EXIT:
    return "exit";
  case JOB_REASON_TIMEOUT:
    return "timeout";
  case JOB_REASON_KILLED:
    return "killed";
  default:
    return "-";
  }
}

static int parse_line(char *line, char **argv, int max_args)
{
  char *src = line;
  char *dst = line;
  char *tok = 0;
  int argc = 0;
  int in_sq = 0;
  int in_dq = 0;
  int esc = 0;

  while(*src){
    char ch = *src++;

    if(esc){
      if(tok == 0)
        tok = dst;
      *dst++ = ch;
      esc = 0;
      continue;
    }

    if(ch == '\\' && !in_sq){
      esc = 1;
      continue;
    }

    if(in_sq){
      if(ch == '\'')
        in_sq = 0;
      else {
        if(tok == 0)
          tok = dst;
        *dst++ = ch;
      }
      continue;
    }

    if(in_dq){
      if(ch == '"')
        in_dq = 0;
      else {
        if(tok == 0)
          tok = dst;
        *dst++ = ch;
      }
      continue;
    }

    if(ch == '\''){
      if(tok == 0)
        tok = dst;
      in_sq = 1;
      continue;
    }

    if(ch == '"'){
      if(tok == 0)
        tok = dst;
      in_dq = 1;
      continue;
    }

    if(ch == ' ' || ch == '\t'){
      if(tok){
        *dst++ = 0;
        if(argc >= max_args)
          return max_args;
        argv[argc++] = tok;
        tok = 0;
      }
      continue;
    }

    if(ch == '|' || ch == '&' || ch == '<' || ch == '>'){
      if(tok){
        *dst++ = 0;
        if(argc >= max_args)
          return max_args;
        argv[argc++] = tok;
        tok = 0;
      }
      if(argc >= max_args)
        return max_args;
      if(ch == '|')
        argv[argc++] = "|";
      else if(ch == '&')
        argv[argc++] = "&";
      else if(ch == '<')
        argv[argc++] = "<";
      else if(*src == '>'){
        src++;
        argv[argc++] = ">>";
      } else
        argv[argc++] = ">";
      continue;
    }

    if(tok == 0)
      tok = dst;
    *dst++ = ch;
  }

  if(esc){
    if(tok == 0)
      tok = dst;
    *dst++ = '\\';
  }

  if(in_sq || in_dq)
    return -1;

  if(tok){
    *dst++ = 0;
    if(argc < max_args)
      argv[argc++] = tok;
  }

  return argc;
}

static void cmd_help(void)
{
  puts_line("commands:");
  puts_line("  help");
  puts_line("  <elf-command> [args]");
  puts_line("  <elf-command> [args] &");
  puts_line("  <a> | <b> | <c> ...");
  puts_line("  redirection: < > >> 2> 2>>");
  puts_line("  cd [dir], pwd");
  puts_line("  env, export NAME=VALUE, unset NAME");
  puts_line("  ps");
  puts_line("  jobs");
  puts_line("  fg <jobid>");
  puts_line("  kill <jobid>");
  puts_line("  wait [jobid]");
  puts_line("  time <cmd...>");
  puts_line("  ulimit [-t ms] [-m kb]");
  puts_line("  limit <ms> <heap_kb> <cmd...> [&]");
  puts_line("  reboot");
}

static int try_read_exec_image(const char *cmd, void **out_image, uint32 *out_size, char *resolved, int resolved_len)
{
  char pathbuf[MAXPATH];
  const char *path_env;
  const char *p;

  if(cmd == 0 || out_image == 0 || out_size == 0)
    return -1;
  *out_image = 0;
  *out_size = 0;
  if(resolved && resolved_len > 0)
    resolved[0] = 0;

  if(strchr(cmd, '/')){
    if(xv6fs_read_file_alloc_path(cmd, out_image, out_size) == 0){
      if(resolved && resolved_len > 0){
        strncpy(resolved, cmd, resolved_len - 1);
        resolved[resolved_len - 1] = 0;
      }
      return 0;
    }
    if(snprintf(pathbuf, sizeof(pathbuf), "%s.elf", cmd) > 0 && xv6fs_read_file_alloc_path(pathbuf, out_image, out_size) == 0){
      if(resolved && resolved_len > 0){
        strncpy(resolved, pathbuf, resolved_len - 1);
        resolved[resolved_len - 1] = 0;
      }
      return 0;
    }
    return -1;
  }

  path_env = env_get("PATH");
  if(path_env == 0 || path_env[0] == 0)
    return -1;

  p = path_env;
  while(1){
    const char *seg = p;
    int seg_len = 0;
    while(*p && *p != ':'){
      p++;
      seg_len++;
    }

    if(seg_len == 0){
      if(snprintf(pathbuf, sizeof(pathbuf), "%s", cmd) > 0 && xv6fs_read_file_alloc_path(pathbuf, out_image, out_size) == 0){
        if(resolved && resolved_len > 0){
          strncpy(resolved, pathbuf, resolved_len - 1);
          resolved[resolved_len - 1] = 0;
        }
        return 0;
      }
      if(snprintf(pathbuf, sizeof(pathbuf), "%s.elf", cmd) > 0 && xv6fs_read_file_alloc_path(pathbuf, out_image, out_size) == 0){
        if(resolved && resolved_len > 0){
          strncpy(resolved, pathbuf, resolved_len - 1);
          resolved[resolved_len - 1] = 0;
        }
        return 0;
      }
    } else {
      if(seg_len >= (int)sizeof(pathbuf))
        seg_len = (int)sizeof(pathbuf) - 1;
      memcpy(pathbuf, seg, (unsigned)seg_len);
      pathbuf[seg_len] = 0;
      if(snprintf(pathbuf + seg_len, sizeof(pathbuf) - (unsigned)seg_len, "/%s", cmd) > 0 &&
         xv6fs_read_file_alloc_path(pathbuf, out_image, out_size) == 0){
        if(resolved && resolved_len > 0){
          strncpy(resolved, pathbuf, resolved_len - 1);
          resolved[resolved_len - 1] = 0;
        }
        return 0;
      }
      if(snprintf(pathbuf + seg_len, sizeof(pathbuf) - (unsigned)seg_len, "/%s.elf", cmd) > 0 &&
         xv6fs_read_file_alloc_path(pathbuf, out_image, out_size) == 0){
        if(resolved && resolved_len > 0){
          strncpy(resolved, pathbuf, resolved_len - 1);
          resolved[resolved_len - 1] = 0;
        }
        return 0;
      }
    }

    if(*p == 0)
      break;
    p++;
  }

  return -1;
}

static char **dup_exec_argv(int argc, char **argv)
{
  char **copy;
  int i;

  if(argc <= 0 || argv == 0)
    return 0;

  copy = (char **)calloc((size_t)argc + 1u, sizeof(char *));
  if(copy == 0)
    return 0;

  for(i = 0; i < argc; i++){
    copy[i] = strdup(argv[i] ? argv[i] : "");
    if(copy[i] == 0){
      int j;
      for(j = 0; j < i; j++)
        free(copy[j]);
      free(copy);
      return 0;
    }
  }
  copy[argc] = 0;
  return copy;
}

static void free_exec_argv(int argc, char **argv)
{
  int i;
  if(argv == 0)
    return;
  for(i = 0; i < argc; i++)
    free(argv[i]);
  free(argv);
}

static int run_elf_command(int argc, char **argv, int *exit_code, int in_fd, int out_fd, int err_fd, int max_heap_kb)
{
  elf_module_t *m;
  void *image = 0;
  uint32 image_size = 0;
  int retv = 0;
  int rc = -1;
  int loader_locked = 0;
  char **exec_argv_owned = 0;
  char **exec_argv = 0;
  char module_name[MAXPATH];

  if(argc <= 0 || argv == 0 || argv[0] == 0 || argv[0][0] == 0)
    return -1;

  if(exit_code)
    *exit_code = 127;

  if(max_heap_kb > 0){
    int free_kb = k_free_heap() / 1024;
    if(free_kb < max_heap_kb){
      puts_line("exec: blocked by memory limit");
      if(exit_code)
        *exit_code = 125;
      return -1;
    }
  }

  exec_argv_owned = dup_exec_argv(argc, argv);
  if(exec_argv_owned == 0){
    puts_line("exec: no memory");
    return -1;
  }
  exec_argv = (char **)calloc((size_t)argc + 1u, sizeof(char *));
  if(exec_argv == 0){
    free_exec_argv(argc, exec_argv_owned);
    puts_line("exec: no memory");
    return -1;
  }
  memcpy(exec_argv, exec_argv_owned, ((size_t)argc + 1u) * sizeof(char *));
  k_optind = 1;
  k_opterr = 1;
  k_optopt = 0;
  k_optarg = 0;
  k_optreset = 0;
  k_getopt_pos = 1;

  xv6_stdio_set_fds(in_fd, out_fd, err_fd);

  if(g_loader_lock){
    (void)xSemaphoreTake(g_loader_lock, portMAX_DELAY);
    loader_locked = 1;
  }
  strncpy(module_name, exec_argv[0], sizeof(module_name) - 1);
  module_name[sizeof(module_name) - 1] = 0;
  if(try_read_exec_image(exec_argv[0], &image, &image_size, module_name, sizeof(module_name)) != 0 || image == 0){
    puts_line("exec: command not found");
    goto out;
  }
  if(g_loaded_module_valid && strcmp(g_loaded_module, module_name) != 0){
    (void)elf_module_unload(g_loaded_module);
    g_loaded_module_valid = 0;
    g_loaded_module[0] = 0;
  }
  if(elf_module_load_from_bytes(module_name, image, image_size, &m) != 0){
    free(image);
    puts_line("exec: elf load failed");
    goto out;
  }
  if(!g_loaded_module_valid || strcmp(g_loaded_module, module_name) != 0){
    strncpy(g_loaded_module, module_name, sizeof(g_loaded_module) - 1);
    g_loaded_module[sizeof(g_loaded_module) - 1] = 0;
    g_loaded_module_valid = 1;
  }
  free(image);

  if(elf_module_call_main(m, argc, exec_argv, &retv) != 0){
    puts_line("exec: entry call failed");
    if(exit_code)
      *exit_code = 126;
    goto out;
  }

  if(exit_code)
    *exit_code = retv;
  if(retv != 0){
    puts_console(exec_argv[0]);
    puts_console(": exit=");
    print_u32((uint32)retv);
    puts_line("");
  }
  rc = 0;

out:
  if(loader_locked && g_loader_lock)
    (void)xSemaphoreGive(g_loader_lock);
  xv6_stdio_reset_fds();
  free(exec_argv);
  free_exec_argv(argc, exec_argv_owned);
  return rc;
}

static int job_find_slot_by_id(int id)
{
  int i;
  for(i = 0; i < KSH_MAX_JOBS; i++){
    if(g_jobs[i].used && g_jobs[i].id == id)
      return i;
  }
  return -1;
}

static void close_job_fd_if_needed(int fd)
{
  if(fd >= 3)
    (void)xv6_close(fd);
}

static void free_job_ctx(ksh_job_task_t *t)
{
  int i;
  if(t == 0)
    return;
  if(t->in_fd >= 3)
    close_job_fd_if_needed(t->in_fd);
  if(t->out_fd >= 3 && t->out_fd != t->in_fd)
    close_job_fd_if_needed(t->out_fd);
  if(t->err_fd >= 3 && t->err_fd != t->in_fd && t->err_fd != t->out_fd)
    close_job_fd_if_needed(t->err_fd);
  for(i = 0; i < t->argc; i++)
    free(t->argv[i]);
  free(t->argv);
  free(t);
}

static void enforce_job_limits(void)
{
  int i;
  uint32 now = (uint32)k_ticks();
  ksh_job_task_t *kill_ctx[KSH_MAX_JOBS];
  int nkill = 0;

  memset(kill_ctx, 0, sizeof(kill_ctx));

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  for(i = 0; i < KSH_MAX_JOBS; i++){
    if(!g_jobs[i].used || g_jobs[i].done)
      continue;
    if(g_jobs[i].max_runtime_ms > 0 && ((uint32)(now - g_jobs[i].started_ms) * 10u) > g_jobs[i].max_runtime_ms){
      TaskHandle_t h = g_jobs[i].task;
      if(h)
        vTaskDelete(h);
      if(nkill < KSH_MAX_JOBS && g_jobs[i].task_ctx)
        kill_ctx[nkill++] = (ksh_job_task_t *)g_jobs[i].task_ctx;
      g_jobs[i].done = 1;
      g_jobs[i].exit_code = 124;
      g_jobs[i].reason = JOB_REASON_TIMEOUT;
      g_jobs[i].task = 0;
      g_jobs[i].task_ctx = 0;
    }
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  for(i = 0; i < nkill; i++){
    if(kill_ctx[i])
      free_job_ctx(kill_ctx[i]);
  }
}

static void job_task(void *arg)
{
  ksh_job_task_t *t = (ksh_job_task_t *)arg;
  int exit_code = 127;

  if(t->cwd[0])
    (void)xv6_chdir(t->cwd);
  (void)run_elf_command(t->argc, t->argv, &exit_code, t->in_fd, t->out_fd, t->err_fd, t->max_heap_kb);

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  if(t->slot >= 0 && t->slot < KSH_MAX_JOBS && g_jobs[t->slot].used){
    g_jobs[t->slot].done = 1;
    g_jobs[t->slot].exit_code = exit_code;
    g_jobs[t->slot].reason = (g_jobs[t->slot].reason == JOB_REASON_NONE) ? JOB_REASON_EXIT : g_jobs[t->slot].reason;
    g_jobs[t->slot].task = 0;
    g_jobs[t->slot].task_ctx = 0;
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  free_job_ctx(t);
  xv6_task_ctx_cleanup();
  vTaskDelete(NULL);
}

static int terminate_job_id(int id, int exit_code, int reason)
{
  int slot;
  TaskHandle_t h = 0;
  ksh_job_task_t *ctx = 0;
  int rc = -1;

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  slot = job_find_slot_by_id(id);
  if(slot >= 0 && g_jobs[slot].used && !g_jobs[slot].done){
    h = g_jobs[slot].task;
    ctx = (ksh_job_task_t *)g_jobs[slot].task_ctx;
    g_jobs[slot].done = 1;
    g_jobs[slot].exit_code = exit_code;
    g_jobs[slot].reason = reason;
    g_jobs[slot].task = 0;
    g_jobs[slot].task_ctx = 0;
    if(h)
      vTaskDelete(h);
    rc = 0;
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(rc != 0)
    return -1;
  if(ctx)
    free_job_ctx(ctx);
  return rc;
}

static int wait_job_id_ex(int id, int *out_exit_code, int consume, int allow_ctrl_c)
{
  while(1){
    int slot;
    int done = 0;
    int exit_code = 0;

    if(g_jobs_lock)
      (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
    slot = job_find_slot_by_id(id);
    if(slot < 0){
      if(g_jobs_lock)
        (void)xSemaphoreGive(g_jobs_lock);
      return -1;
    }

    if(g_jobs[slot].done){
      done = 1;
      exit_code = g_jobs[slot].exit_code;
      if(consume)
        memset(&g_jobs[slot], 0, sizeof(g_jobs[slot]));
    }
    if(g_jobs_lock)
      (void)xSemaphoreGive(g_jobs_lock);

    if(done){
      if(out_exit_code)
        *out_exit_code = exit_code;
      return 0;
    }

    if(allow_ctrl_c){
      int c = hal_console_getc();
      if(c == 0x03){
        if(terminate_job_id(id, 130, JOB_REASON_KILLED) == 0){
          puts_line("^C");
          if(out_exit_code)
            *out_exit_code = 130;
          if(consume){
            if(g_jobs_lock)
              (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
            slot = job_find_slot_by_id(id);
            if(slot >= 0)
              memset(&g_jobs[slot], 0, sizeof(g_jobs[slot]));
            if(g_jobs_lock)
              (void)xSemaphoreGive(g_jobs_lock);
          }
          return 0;
        }
      }
    }

    enforce_job_limits();
    hal_delay_ms(10);
  }
}

static int wait_job_id(int id, int *out_exit_code, int consume)
{
  return wait_job_id_ex(id, out_exit_code, consume, 0);
}

static int spawn_background_ex(int argc, char **argv, int in_fd, int out_fd, int err_fd, int is_pipe, int quiet_start,
                               int max_heap_kb, uint32 max_runtime_ms, int *out_job_id)
{
  int i, j;
  int slot = -1;
  int id = 0;
  int pos = 0;
  TaskHandle_t handle = 0;
  ksh_job_task_t *t = 0;

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  for(i = 0; i < KSH_MAX_JOBS; i++){
    if(!g_jobs[i].used){
      slot = i;
      break;
    }
  }
  if(slot < 0){
    if(g_jobs_lock)
      (void)xSemaphoreGive(g_jobs_lock);
    puts_line("jobs: table full");
    return -1;
  }

  t = (ksh_job_task_t *)calloc(1, sizeof(*t));
  if(t == 0){
    if(g_jobs_lock)
      (void)xSemaphoreGive(g_jobs_lock);
    puts_line("jobs: no memory");
    return -1;
  }

  t->argv = (char **)calloc((unsigned)argc + 1, sizeof(char *));
  if(t->argv == 0){
    if(g_jobs_lock)
      (void)xSemaphoreGive(g_jobs_lock);
    free(t);
    puts_line("jobs: no memory");
    return -1;
  }

  t->slot = slot;
  t->argc = argc;
  t->in_fd = in_fd;
  t->out_fd = out_fd;
  t->err_fd = err_fd;
  t->max_heap_kb = max_heap_kb;
  if(xv6_getcwd(t->cwd, sizeof(t->cwd)) != 0)
    strcpy(t->cwd, "/");

  if(t->in_fd >= 3){
    t->in_fd = xv6_dup(t->in_fd);
    if(t->in_fd < 0){
      free(t->argv);
      free(t);
      if(g_jobs_lock)
        (void)xSemaphoreGive(g_jobs_lock);
      puts_line("jobs: fd dup failed");
      return -1;
    }
  }
  if(t->out_fd >= 3){
    int dupfd = xv6_dup(t->out_fd);
    if(dupfd < 0){
      free_job_ctx(t);
      if(g_jobs_lock)
        (void)xSemaphoreGive(g_jobs_lock);
      puts_line("jobs: fd dup failed");
      return -1;
    }
    t->out_fd = dupfd;
  }
  if(t->err_fd >= 3){
    int dupfd = xv6_dup(t->err_fd);
    if(dupfd < 0){
      free_job_ctx(t);
      if(g_jobs_lock)
        (void)xSemaphoreGive(g_jobs_lock);
      puts_line("jobs: fd dup failed");
      return -1;
    }
    t->err_fd = dupfd;
  }

  for(i = 0; i < argc; i++){
    size_t n = strlen(argv[i]) + 1;
    t->argv[i] = (char *)malloc(n);
    if(t->argv[i] == 0){
      for(j = 0; j < i; j++)
        free(t->argv[j]);
      free(t->argv);
      free(t);
      if(g_jobs_lock)
        (void)xSemaphoreGive(g_jobs_lock);
      puts_line("jobs: no memory");
      return -1;
    }
    memcpy(t->argv[i], argv[i], n);
  }
  t->argv[argc] = 0;

  id = g_next_job_id++;
  if(g_next_job_id < 1)
    g_next_job_id = 1;

  memset(&g_jobs[slot], 0, sizeof(g_jobs[slot]));
  g_jobs[slot].used = 1;
  g_jobs[slot].id = id;
  g_jobs[slot].is_pipe = is_pipe;
  g_jobs[slot].max_heap_kb = max_heap_kb;
  g_jobs[slot].max_runtime_ms = max_runtime_ms;
  g_jobs[slot].started_ms = (uint32)k_ticks();
  g_jobs[slot].reason = JOB_REASON_NONE;
  g_jobs[slot].task_ctx = t;

  for(i = 0; i < argc; i++){
    int n = snprintf(g_jobs[slot].cmd + pos, sizeof(g_jobs[slot].cmd) - (unsigned)pos, "%s%s", (i ? " " : ""),
                     argv[i]);
    if(n <= 0 || pos + n >= (int)sizeof(g_jobs[slot].cmd)){
      g_jobs[slot].cmd[sizeof(g_jobs[slot].cmd) - 1] = 0;
      break;
    }
    pos += n;
  }

  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(xTaskCreate(job_task, "xv6_bg", KSH_BG_STACK, t, 5, &handle) != pdPASS){
    if(g_jobs_lock)
      (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
    memset(&g_jobs[slot], 0, sizeof(g_jobs[slot]));
    if(g_jobs_lock)
      (void)xSemaphoreGive(g_jobs_lock);
    free_job_ctx(t);
    puts_line("jobs: spawn failed");
    return -1;
  }

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  if(g_jobs[slot].used)
    g_jobs[slot].task = handle;
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(!quiet_start){
    putc_console('[');
    print_u32((uint32)id);
    puts_line("] started");
  }

  if(out_job_id)
    *out_job_id = id;
  return 0;
}

static int run_foreground_with_limits(int argc, char **argv, int in_fd, int out_fd, int err_fd, int max_heap_kb,
                                      uint32 max_runtime_ms)
{
  int rc;
  int job_id = -1;
  int exit_code = 127;

  rc = spawn_background_ex(argc, argv, in_fd, out_fd, err_fd, 0, 1, max_heap_kb, max_runtime_ms, &job_id);
  if(rc != 0)
    return -1;
  rc = wait_job_id_ex(job_id, &exit_code, 1, 1);
  if(rc != 0)
    return -1;
  if(exit_code == 124){
    puts_line("limit: timeout");
    return -1;
  }
  return exit_code == 0 ? 0 : -1;
}

static int is_fd_token(const char *s, int *out_fd)
{
  if(s && s[0] >= '0' && s[0] <= '2' && s[1] == 0){
    if(out_fd)
      *out_fd = (int)(s[0] - '0');
    return 1;
  }
  return 0;
}

static int is_redir_token(const char *s)
{
  return (strcmp(s, "<") == 0 || strcmp(s, ">") == 0 || strcmp(s, ">>") == 0);
}

static int is_control_token(const char *s)
{
  return (strcmp(s, "|") == 0 || strcmp(s, "&") == 0 || is_redir_token(s));
}

static void close_io_custom_fds(const ksh_io_t *io, const ksh_io_t *base)
{
  int base_fds[3];
  int vals[3];
  int i, j;

  if(io == 0 || base == 0)
    return;

  base_fds[0] = base->in_fd;
  base_fds[1] = base->out_fd;
  base_fds[2] = base->err_fd;
  vals[0] = io->in_fd;
  vals[1] = io->out_fd;
  vals[2] = io->err_fd;

  for(i = 0; i < 3; i++){
    int is_base = 0;
    if(vals[i] < 3)
      continue;
    for(j = 0; j < 3; j++){
      if(vals[i] == base_fds[j]){
        is_base = 1;
        break;
      }
    }
    if(is_base)
      continue;
    for(j = 0; j < i; j++){
      if(vals[i] == vals[j]){
        is_base = 1;
        break;
      }
    }
    if(!is_base)
      (void)xv6_close(vals[i]);
  }
}

static int parse_exec_and_redir(int argc, char **argv, const ksh_io_t *base_io, char **exec_argv, int max_exec,
                                int *out_argc, ksh_io_t *out_io)
{
  int i;
  int n = 0;
  ksh_io_t io;

  if(base_io == 0 || exec_argv == 0 || out_argc == 0 || out_io == 0)
    return -1;

  io = *base_io;
  for(i = 0; i < argc; i++){
    int target_fd;
    int flags;
    int fd;
    int *dst;
    const char *op = argv[i];
    const char *path;

    if(!is_redir_token(op)){
      if(strcmp(op, "|") == 0 || strcmp(op, "&") == 0){
        puts_line("syntax: bad token");
        goto fail;
      }
      if(n >= max_exec - 1){
        puts_line("exec: too many args");
        goto fail;
      }
      exec_argv[n++] = argv[i];
      continue;
    }

    target_fd = (op[0] == '<') ? 0 : 1;
    if(n > 0 && is_fd_token(exec_argv[n - 1], &target_fd))
      n--;
    if(op[0] == '<' && target_fd != 0){
      puts_line("redir: bad input fd");
      goto fail;
    }

    if(i + 1 >= argc){
      puts_line("redir: missing path");
      goto fail;
    }
    path = argv[++i];
    if(is_control_token(path)){
      puts_line("redir: bad path");
      goto fail;
    }

    if(op[0] == '<')
      flags = XV6_O_RDONLY;
    else if(strcmp(op, ">>") == 0)
      flags = XV6_O_WRONLY | XV6_O_CREAT | XV6_O_APPEND;
    else
      flags = XV6_O_WRONLY | XV6_O_CREAT | XV6_O_TRUNC;

    fd = xv6_open(path, flags);
    if(fd < 0){
      puts_console("redir: open failed: ");
      puts_line(path);
      goto fail;
    }

    if(target_fd == 0)
      dst = &io.in_fd;
    else if(target_fd == 1)
      dst = &io.out_fd;
    else if(target_fd == 2)
      dst = &io.err_fd;
    else {
      xv6_close(fd);
      puts_line("redir: bad fd");
      goto fail;
    }

    if(*dst >= 3 && *dst != base_io->in_fd && *dst != base_io->out_fd && *dst != base_io->err_fd)
      xv6_close(*dst);
    *dst = fd;
  }

  if(n <= 0){
    puts_line("syntax: empty command");
    goto fail;
  }
  exec_argv[n] = 0;
  *out_argc = n;
  *out_io = io;
  return 0;

fail:
  close_io_custom_fds(&io, base_io);
  return -1;
}

static int find_pipe_pos(int argc, char **argv)
{
  int i;
  for(i = 0; i < argc; i++){
    if(strcmp(argv[i], "|") == 0)
      return i;
  }
  return -1;
}

static int build_pipeline(int argc, char **argv, int *starts, int *lens, int max_stages)
{
  int i;
  int stage = 0;
  int start = 0;

  for(i = 0; i < argc; i++){
    if(strcmp(argv[i], "|") != 0)
      continue;
    if(i == start || stage >= max_stages)
      return -1;
    starts[stage] = start;
    lens[stage] = i - start;
    stage++;
    start = i + 1;
  }

  if(start >= argc || stage >= max_stages)
    return -1;
  starts[stage] = start;
  lens[stage] = argc - start;
  stage++;
  return stage;
}

static int run_pipeline(int argc, char **argv, int run_bg, int max_heap_kb, uint32 max_runtime_ms)
{
  int stage_starts[KSH_MAX_STAGES];
  int stage_lens[KSH_MAX_STAGES];
  int stage_count;
  int pipe_r[KSH_MAX_STAGES - 1];
  int pipe_w[KSH_MAX_STAGES - 1];
  int jobs[KSH_MAX_STAGES];
  int njobs = 0;
  int i;
  int final_rc = 0;

  for(i = 0; i < KSH_MAX_STAGES - 1; i++){
    pipe_r[i] = -1;
    pipe_w[i] = -1;
  }

  stage_count = build_pipeline(argc, argv, stage_starts, stage_lens, KSH_MAX_STAGES);
  if(stage_count < 2){
    puts_line("pipe: syntax");
    return -1;
  }

  for(i = 0; i < stage_count - 1; i++){
    if(xv6_pipe(&pipe_r[i], &pipe_w[i]) != 0){
      puts_line("pipe: alloc failed");
      goto fail;
    }
  }

  for(i = 0; i < stage_count; i++){
    char *stage_exec[KSH_MAX_ARGS];
    int stage_exec_argc = 0;
    int base_in = (i == 0) ? 0 : pipe_r[i - 1];
    int base_out = (i == stage_count - 1) ? 1 : pipe_w[i];
    ksh_io_t base_io;
    ksh_io_t io;
    int sid = -1;
    int stage_argc = stage_lens[i];
    char **stage_argv = &argv[stage_starts[i]];

    base_io.in_fd = base_in;
    base_io.out_fd = base_out;
    base_io.err_fd = 2;
    if(parse_exec_and_redir(stage_argc, stage_argv, &base_io, stage_exec, KSH_MAX_ARGS, &stage_exec_argc, &io) != 0)
      goto fail;

    if(i > 0 && io.in_fd != base_in && pipe_r[i - 1] >= 3){
      xv6_close(pipe_r[i - 1]);
      pipe_r[i - 1] = -1;
    }
    if(i < stage_count - 1 && io.out_fd != base_out && pipe_w[i] >= 3){
      xv6_close(pipe_w[i]);
      pipe_w[i] = -1;
    }

    if(i == stage_count - 1 && !run_bg){
      final_rc =
        run_foreground_with_limits(stage_exec_argc, stage_exec, io.in_fd, io.out_fd, io.err_fd, max_heap_kb, max_runtime_ms);
    } else {
      if(spawn_background_ex(stage_exec_argc, stage_exec, io.in_fd, io.out_fd, io.err_fd, 1, run_bg ? 0 : 1, max_heap_kb,
                             max_runtime_ms, &sid) != 0)
      {
        close_io_custom_fds(&io, &base_io);
        goto fail;
      }
      jobs[njobs++] = sid;
    }
    close_io_custom_fds(&io, &base_io);

    if(i > 0 && pipe_r[i - 1] >= 3){
      xv6_close(pipe_r[i - 1]);
      pipe_r[i - 1] = -1;
    }
    if(i < stage_count - 1 && pipe_w[i] >= 3){
      xv6_close(pipe_w[i]);
      pipe_w[i] = -1;
    }
  }

  if(!run_bg){
    for(i = 0; i < njobs; i++){
      int ignore = 0;
      (void)wait_job_id(jobs[i], &ignore, 1);
    }
  }

  return final_rc;

fail:
  for(i = 0; i < KSH_MAX_STAGES - 1; i++){
    if(pipe_r[i] >= 3)
      xv6_close(pipe_r[i]);
    if(pipe_w[i] >= 3)
      xv6_close(pipe_w[i]);
  }
  return -1;
}

static int execute_external(int argc, char **argv, int run_bg, int max_heap_kb, uint32 max_runtime_ms)
{
  char *exec_argv[KSH_MAX_ARGS];
  int exec_argc = 0;
  int rc;
  ksh_io_t base_io;
  ksh_io_t io;

  if(find_pipe_pos(argc, argv) >= 0)
    return run_pipeline(argc, argv, run_bg, max_heap_kb, max_runtime_ms);

  base_io.in_fd = 0;
  base_io.out_fd = 1;
  base_io.err_fd = 2;

  if(parse_exec_and_redir(argc, argv, &base_io, exec_argv, KSH_MAX_ARGS, &exec_argc, &io) != 0)
    return -1;

  if(run_bg)
    rc = spawn_background_ex(exec_argc, exec_argv, io.in_fd, io.out_fd, io.err_fd, 0, 0, max_heap_kb, max_runtime_ms, 0);
  else
    rc = run_foreground_with_limits(exec_argc, exec_argv, io.in_fd, io.out_fd, io.err_fd, max_heap_kb, max_runtime_ms);

  close_io_custom_fds(&io, &base_io);
  return rc;
}

static void cmd_jobs(void)
{
  int i;
  int any = 0;

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  for(i = 0; i < KSH_MAX_JOBS; i++){
    if(!g_jobs[i].used)
      continue;
    k_printf("[%d] %s %s\r\n", g_jobs[i].id, g_jobs[i].done ? "done" : "running", g_jobs[i].cmd);
    any = 1;
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(!any)
    puts_line("jobs: empty");
}

static void cmd_ps(void)
{
  int i;
  int any = 0;

  k_printf("PID STATE EXIT REASON LIMIT(ms/kb) CMD\r\n");
  k_printf("0 RUN - - - ksh\r\n");

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  for(i = 0; i < KSH_MAX_JOBS; i++){
    if(!g_jobs[i].used)
      continue;
    if(g_jobs[i].done){
      k_printf("%d DONE %d %s %u/%d %s\r\n", g_jobs[i].id, g_jobs[i].exit_code, job_reason_str(g_jobs[i].reason),
               (unsigned)g_jobs[i].max_runtime_ms, g_jobs[i].max_heap_kb, g_jobs[i].cmd);
    } else {
      k_printf("%d RUN - - %u/%d %s\r\n", g_jobs[i].id, (unsigned)g_jobs[i].max_runtime_ms, g_jobs[i].max_heap_kb,
               g_jobs[i].cmd);
    }
    any = 1;
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(!any)
    k_printf("- NOJOBS -\r\n");
}

static void cmd_wait(int argc, char **argv)
{
  if(argc == 1){
    while(1){
      int i;
      int has_used = 0;
      int has_running = 0;
      if(g_jobs_lock)
        (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
      for(i = 0; i < KSH_MAX_JOBS; i++){
        if(!g_jobs[i].used)
          continue;
        has_used = 1;
        if(g_jobs[i].done)
          memset(&g_jobs[i], 0, sizeof(g_jobs[i]));
        else
          has_running = 1;
      }
      if(g_jobs_lock)
        (void)xSemaphoreGive(g_jobs_lock);
      if(!has_used || !has_running)
        break;
      enforce_job_limits();
      hal_delay_ms(10);
    }
    puts_line("wait: done");
    return;
  }

  if(argc == 2){
    uint32 id = 0;
    int exit_code = 0;
    if(parse_u32_dec(argv[1], &id) != 0){
      puts_line("wait: bad job id");
      return;
    }
    if(wait_job_id((int)id, &exit_code, 1) != 0){
      puts_line("wait: no such job");
      return;
    }
    k_printf("wait: done %u\r\n", (unsigned)exit_code);
    return;
  }

  puts_line("usage: wait [jobid]");
}

static void cmd_kill(int argc, char **argv)
{
  uint32 id = 0;

  if(argc != 2 || parse_u32_dec(argv[1], &id) != 0){
    puts_line("usage: kill <jobid>");
    return;
  }

  if(terminate_job_id((int)id, 137, JOB_REASON_KILLED) != 0){
    puts_line("kill: no such job");
    return;
  }

  puts_line("kill: ok");
}

static void cmd_fg(int argc, char **argv)
{
  uint32 id = 0;
  int exit_code = 0;

  if(argc != 2 || parse_u32_dec(argv[1], &id) != 0){
    puts_line("usage: fg <jobid>");
    return;
  }

  if(wait_job_id((int)id, &exit_code, 1) != 0){
    puts_line("fg: no such job");
    return;
  }

  k_printf("fg: done %u\r\n", (unsigned)exit_code);
}

static void cmd_ulimit(int argc, char **argv)
{
  uint32 v = 0;

  if(argc == 1){
    k_printf("ulimit: -t %u ms, -m %d kb\r\n", (unsigned)g_ulimit_ms, g_ulimit_heap_kb);
    return;
  }

  if(argc == 2){
    if(strcmp(argv[1], "-t") == 0){
      k_printf("%u\r\n", (unsigned)g_ulimit_ms);
      return;
    }
    if(strcmp(argv[1], "-m") == 0){
      k_printf("%d\r\n", g_ulimit_heap_kb);
      return;
    }
    puts_line("usage: ulimit [-t ms] [-m kb]");
    return;
  }

  if(argc == 3){
    if(parse_u32_dec(argv[2], &v) != 0){
      puts_line("ulimit: bad value");
      return;
    }
    if(strcmp(argv[1], "-t") == 0){
      g_ulimit_ms = v;
      return;
    }
    if(strcmp(argv[1], "-m") == 0){
      g_ulimit_heap_kb = (int)v;
      return;
    }
  }

  puts_line("usage: ulimit [-t ms] [-m kb]");
}

static void cmd_limit(int argc, char **argv, int run_bg)
{
  uint32 max_ms = 0;
  uint32 max_kb = 0;

  if(argc < 4){
    puts_line("usage: limit <ms> <heap_kb> <cmd...>");
    return;
  }
  if(parse_u32_dec(argv[1], &max_ms) != 0 || parse_u32_dec(argv[2], &max_kb) != 0){
    puts_line("limit: bad numeric args");
    return;
  }

  (void)execute_external(argc - 3, argv + 3, run_bg, (int)max_kb, max_ms);
}

static void cmd_time(int argc, char **argv, int run_bg)
{
  uint32 start;
  uint32 end;
  if(argc < 2){
    puts_line("usage: time <cmd...>");
    return;
  }

  start = (uint32)k_ticks();
  (void)dispatch_command(argc - 1, argv + 1, run_bg);
  end = (uint32)k_ticks();

  if(!run_bg)
    k_eprintf("time: %u ms\r\n", (unsigned)((end - start) * 10u));
}

static void cmd_pwd(void)
{
  char cwd[MAXPATH];
  if(xv6_getcwd(cwd, sizeof(cwd)) != 0){
    eputs_line("pwd: failed");
    return;
  }
  puts_line(cwd);
}

static void cmd_cd(int argc, char **argv)
{
  const char *path = 0;

  if(argc > 2){
    eputs_line("usage: cd [dir]");
    return;
  }
  if(argc == 2)
    path = argv[1];
  else
    path = env_get("HOME");
  if(path == 0 || path[0] == 0)
    path = "/";
  if(xv6_chdir(path) != 0){
    eputs_console("cd: failed: ");
    eputs_line(path);
    return;
  }
  env_sync_pwd();
}

static void cmd_env(void)
{
  int i;
  for(i = 0; i < KSH_MAX_ENV; i++){
    if(!g_env[i].used)
      continue;
    k_printf("%s=%s\r\n", g_env[i].key, g_env[i].val);
  }
}

static void cmd_export(int argc, char **argv)
{
  int i;

  if(argc == 1){
    cmd_env();
    return;
  }
  for(i = 1; i < argc; i++){
    char *eq = strchr(argv[i], '=');
    if(eq){
      char key[KSH_ENV_KEY];
      int n = (int)(eq - argv[i]);
      if(n <= 0 || n >= (int)sizeof(key)){
        eputs_console("export: bad name: ");
        eputs_line(argv[i]);
        continue;
      }
      memcpy(key, argv[i], (unsigned)n);
      key[n] = 0;
      if(env_set(key, eq + 1) != 0){
        eputs_console("export: bad assignment: ");
        eputs_line(argv[i]);
      }
    } else {
      if(env_set(argv[i], "") != 0){
        eputs_console("export: bad name: ");
        eputs_line(argv[i]);
      }
    }
  }
}

static void cmd_unset(int argc, char **argv)
{
  int i;
  if(argc < 2){
    eputs_line("usage: unset NAME...");
    return;
  }
  for(i = 1; i < argc; i++){
    if(strcmp(argv[i], "PWD") == 0)
      continue;
    env_unset(argv[i]);
  }
}

static void register_default_symbols(void)
{
  static const elf_host_symbol_t syms[] = {
    { "puts", (void *)k_host_puts },
    { "printf", (void *)k_host_printf },
    { "fprintf", (void *)k_host_fprintf },
    { "vfprintf", (void *)k_host_vfprintf },
    { "sprintf", (void *)sprintf },
    { "snprintf", (void *)snprintf },
    { "vsprintf", (void *)vsprintf },
    { "vsnprintf", (void *)vsnprintf },
    { "fputs", (void *)k_fputs },
    { "fputc", (void *)k_fputc },
    { "fopen", (void *)k_fopen },
    { "freopen", (void *)k_freopen },
    { "fclose", (void *)k_fclose },
    { "fgets", (void *)k_fgets },
    { "fgetc", (void *)k_fgetc },
    { "getc", (void *)k_getc },
    { "getchar", (void *)k_getchar },
    { "fread", (void *)k_fread },
    { "fwrite", (void *)k_fwrite },
    { "setbuf", (void *)k_setbuf },
    { "setvbuf", (void *)k_setvbuf },
    { "putchar", (void *)putchar },
    { "putc", (void *)k_putc },
    { "fflush", (void *)k_fflush },
    { "clearerr", (void *)k_clearerr },
    { "feof", (void *)k_feof },
    { "ferror", (void *)k_ferror },
    { "fseek", (void *)k_fseek },
    { "ftell", (void *)k_ftell },
    { "rewind", (void *)k_rewind },
    { "ungetc", (void *)k_ungetc },
    { "perror", (void *)perror },
    { "strerror", (void *)strerror },
    { "fileno", (void *)k_fileno },
    { "exit", (void *)k_exit },
    { "abort", (void *)k_abort },
    { "malloc", (void *)malloc },
    { "calloc", (void *)calloc },
    { "realloc", (void *)realloc },
    { "free", (void *)free },
    { "memset", (void *)memset },
    { "memcpy", (void *)k_host_memcpy },
    { "bcmp", (void *)k_bcmp },
    { "bcopy", (void *)k_bcopy },
    { "bzero", (void *)k_bzero },
    { "index", (void *)strchr },
    { "rindex", (void *)strrchr },
    { "strlen", (void *)k_host_strlen },
    { "strcmp", (void *)k_host_strcmp },
    { "usleep", (void *)usleep },
    { "k_ticks", (void *)k_ticks },
    { "k_puts", (void *)k_puts },
    { "k_free_heap", (void *)k_free_heap },
    { "xv6fs_readdir_path", (void *)k_fs_readdir_path },
    { "xv6fs_read_file_alloc_path", (void *)xv6fs_read_file_alloc_path },
    { "xv6fs_write_file_path", (void *)xv6fs_write_file_path },
    { "xv6fs_mkdir_path", (void *)xv6fs_mkdir_path },
    { "xv6fs_unlink_path", (void *)xv6fs_unlink_path },
    { "xv6_open", (void *)xv6_open },
    { "xv6_dup", (void *)xv6_dup },
    { "xv6_read", (void *)xv6_read },
    { "xv6_write", (void *)xv6_write },
    { "xv6_close", (void *)xv6_close },
    { "xv6_chdir", (void *)xv6_chdir },
    { "xv6_getcwd", (void *)xv6_getcwd },
    { "xv6_ptsname", (void *)xv6_ptsname },
    { "xv6_pipe", (void *)xv6_pipe },
    { "open", (void *)k_open },
    { "creat", (void *)k_creat },
    { "read", (void *)k_read },
    { "write", (void *)k_write },
    { "close", (void *)k_close },
    { "dup", (void *)k_dup },
    { "dup2", (void *)k_dup2 },
    { "lseek", (void *)k_lseek },
    { "stat", (void *)k_stat },
    { "lstat", (void *)k_lstat },
    { "fstat", (void *)k_fstat },
    { "access", (void *)k_access },
    { "mkdir", (void *)k_mkdir },
    { "unlink", (void *)k_unlink },
    { "rmdir", (void *)k_rmdir },
    { "chmod", (void *)k_chmod },
    { "chdir", (void *)k_chdir },
    { "getcwd", (void *)k_getcwd },
    { "isatty", (void *)k_isatty },
    { "utimes", (void *)k_utimes },
    { "lutimes", (void *)k_lutimes },
    { "umask", (void *)k_umask },
    { "sync", (void *)k_sync },
    { "fsync", (void *)k_fsync },
    { "fdatasync", (void *)k_fdatasync },
    { "ftruncate", (void *)k_ftruncate },
    { "truncate", (void *)k_truncate },
    { "link", (void *)k_link },
    { "rename", (void *)k_rename },
    { "symlink", (void *)k_symlink },
    { "readlink", (void *)k_readlink },
    { "mknod", (void *)k_mknod },
    { "mkfifo", (void *)k_mkfifo },
    { "signal", (void *)k_signal },
    { "sigaction", (void *)k_sigaction },
    { "sigprocmask", (void *)k_sigprocmask },
    { "sigemptyset", (void *)k_sigemptyset },
    { "sigfillset", (void *)k_sigfillset },
    { "sigaddset", (void *)k_sigaddset },
    { "sigdelset", (void *)k_sigdelset },
    { "sigismember", (void *)k_sigismember },
    { "raise", (void *)k_raise },
    { "sleep", (void *)k_sleep },
    { "_read_r", (void *)k__read_r },
    { "_write_r", (void *)k__write_r },
    { "_open_r", (void *)k__open_r },
    { "_close_r", (void *)k__close_r },
    { "_lseek_r", (void *)k__lseek_r },
    { "_fstat_r", (void *)k__fstat_r },
    { "_stat_r", (void *)k__stat_r },
    { "_isatty_r", (void *)k__isatty_r },
    { "_unlink_r", (void *)k__unlink_r },
    { "_kill_r", (void *)k__kill_r },
    { "_getpid_r", (void *)k__getpid_r },
    { "_sbrk_r", (void *)k__sbrk_r },
    { "_exit", (void *)k__exit },
    { "fchmod", (void *)k_fchmod },
    { "chown", (void *)k_chown },
    { "lchown", (void *)k_lchown },
    { "fchown", (void *)k_fchown },
    { "getopt", (void *)k_getopt },
    { "dirfd", (void *)dirfd },
    { "optind", (void *)&k_optind },
    { "opterr", (void *)&k_opterr },
    { "optopt", (void *)&k_optopt },
    { "optarg", (void *)&k_optarg },
    { "optreset", (void *)&k_optreset },
    { "__getreent", (void *)__getreent },
  };

  /*
   * Register broad libc exports first, then override with xv6 host shims.
   * resolve_host_symbol() prefers the last registered non-null symbol.
   */
  (void)ksh_register_libc_host_symbols();
  (void)elf_loader_register_host_symbols(syms, (int)(sizeof(syms) / sizeof(syms[0])));
}

static int is_builtin_command(const char *cmd)
{
  return (strcmp(cmd, "help") == 0 || strcmp(cmd, "reboot") == 0 || strcmp(cmd, "cd") == 0 ||
          strcmp(cmd, "pwd") == 0 || strcmp(cmd, "env") == 0 || strcmp(cmd, "export") == 0 ||
          strcmp(cmd, "unset") == 0 || strcmp(cmd, "ps") == 0 || strcmp(cmd, "jobs") == 0 ||
          strcmp(cmd, "wait") == 0 || strcmp(cmd, "kill") == 0 || strcmp(cmd, "fg") == 0 ||
          strcmp(cmd, "time") == 0 || strcmp(cmd, "ulimit") == 0 || strcmp(cmd, "limit") == 0);
}

static int dispatch_builtin_command(int argc, char **argv, int run_bg)
{
  if(strcmp(argv[0], "help") == 0){
    cmd_help();
    return 0;
  }
  if(strcmp(argv[0], "reboot") == 0){
    puts_line("rebooting...");
    hal_reboot();
    return 0;
  }
  if(strcmp(argv[0], "cd") == 0){
    cmd_cd(argc, argv);
    return 0;
  }
  if(strcmp(argv[0], "pwd") == 0){
    cmd_pwd();
    return 0;
  }
  if(strcmp(argv[0], "env") == 0){
    cmd_env();
    return 0;
  }
  if(strcmp(argv[0], "export") == 0){
    cmd_export(argc, argv);
    return 0;
  }
  if(strcmp(argv[0], "unset") == 0){
    cmd_unset(argc, argv);
    return 0;
  }
  if(strcmp(argv[0], "ps") == 0){
    cmd_ps();
    return 0;
  }
  if(strcmp(argv[0], "jobs") == 0){
    cmd_jobs();
    return 0;
  }
  if(strcmp(argv[0], "wait") == 0){
    cmd_wait(argc, argv);
    return 0;
  }
  if(strcmp(argv[0], "kill") == 0){
    cmd_kill(argc, argv);
    return 0;
  }
  if(strcmp(argv[0], "fg") == 0){
    cmd_fg(argc, argv);
    return 0;
  }
  if(strcmp(argv[0], "time") == 0){
    cmd_time(argc, argv, run_bg);
    return 0;
  }
  if(strcmp(argv[0], "ulimit") == 0){
    cmd_ulimit(argc, argv);
    return 0;
  }
  if(strcmp(argv[0], "limit") == 0){
    cmd_limit(argc, argv, run_bg);
    return 0;
  }
  return -1;
}

static int dispatch_command(int argc, char **argv, int run_bg)
{
  char *cmd_argv[KSH_MAX_ARGS];
  int cmd_argc = 0;
  ksh_io_t base_io;
  ksh_io_t io;

  if(argc <= 0)
    return 0;

  if(is_builtin_command(argv[0])){
    base_io.in_fd = 0;
    base_io.out_fd = 1;
    base_io.err_fd = 2;
    if(parse_exec_and_redir(argc, argv, &base_io, cmd_argv, KSH_MAX_ARGS, &cmd_argc, &io) != 0)
      return 0;
    xv6_stdio_set_fds(io.in_fd, io.out_fd, io.err_fd);
    (void)dispatch_builtin_command(cmd_argc, cmd_argv, run_bg);
    xv6_stdio_reset_fds();
    close_io_custom_fds(&io, &base_io);
    return 0;
  }

  (void)execute_external(argc, argv, run_bg, g_ulimit_heap_kb, g_ulimit_ms);
  return 0;
}

void ksh_run(void)
{
  char line[256];
  int len = 0;

  if(__sync_lock_test_and_set(&g_ksh_started, 1) != 0)
    return;

  elf_loader_init();
  g_jobs_lock = xSemaphoreCreateMutex();
  g_loader_lock = xSemaphoreCreateMutex();
  register_default_symbols();
  xv6_vfs_reset();
  (void)xv6_chdir("/");
  env_init_defaults();

  puts_line("xv6-esp32s3 ksh ready");
  cmd_help();
  tty_puts("xv6> ");

  while(1){
    int c;

    enforce_job_limits();

    c = hal_console_getc();
    if(c < 0){
      hal_delay_ms(5);
      continue;
    }

    if(c != 0x03 && c != '\r' && c != '\n' && c != '\b' && c != 0x7f && c != '\t' && (c < 0x20 || c > 0x7e)){
      taskYIELD();
      continue;
    }

    if(c == 0x03){
      puts_line("^C");
      len = 0;
      tty_puts("xv6> ");
      continue;
    }

    if(c == '\r' || c == '\n'){
      char *argv[KSH_MAX_ARGS];
      int argc;
      int run_bg = 0;

      line[len] = 0;
      puts_line("");

      argc = parse_line(line, argv, KSH_MAX_ARGS);
      if(argc < 0){
        puts_line("parse: unterminated quote");
        tty_puts("xv6> ");
        len = 0;
        continue;
      }
      if(argc == 0){
        tty_puts("xv6> ");
        len = 0;
        continue;
      }

      if(strcmp(argv[argc - 1], "&") == 0){
        run_bg = 1;
        argc--;
        if(argc == 0){
          puts_line("syntax: command &");
          tty_puts("xv6> ");
          len = 0;
          continue;
        }
      }

      if(dispatch_command(argc, argv, run_bg) != 0)
        puts_line("unknown command");

      len = 0;
      tty_puts("xv6> ");
      continue;
    }

    if(c == 0x7f || c == '\b'){
      if(len > 0){
        len--;
        tty_puts("\b \b");
      }
      continue;
    }

    if(len < (int)(sizeof(line) - 1)){
      line[len++] = (char)c;
      tty_putc(c);
    }
    taskYIELD();
  }
}
