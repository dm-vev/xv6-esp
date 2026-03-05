static int shell_allow_ctrl_c_poll(void)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  if(self == 0 || g_interactive_task == 0)
    return 0;
  return (self == g_interactive_task) ? 1 : 0;
}

#ifndef POLLIN
#define POLLIN 0x0001
#endif
#ifndef POLLOUT
#define POLLOUT 0x0004
#endif
#ifndef POLLERR
#define POLLERR 0x0008
#endif
#ifndef POLLHUP
#define POLLHUP 0x0010
#endif

#ifndef WNOHANG
#define WNOHANG 1
#endif
#ifndef WUNTRACED
#define WUNTRACED 2
#endif
#ifndef WCONTINUED
#define WCONTINUED 8
#endif
#ifndef WEXITED
#define WEXITED 4
#endif
#ifndef WSTOPPED
#define WSTOPPED WUNTRACED
#endif
#ifndef WNOWAIT
#define WNOWAIT 0x01000000
#endif

#ifndef P_ALL
#define P_ALL 0
#endif
#ifndef P_PID
#define P_PID 1
#endif
#ifndef P_PGID
#define P_PGID 2
#endif

#ifndef CLD_EXITED
#define CLD_EXITED 1
#endif
#ifndef CLD_KILLED
#define CLD_KILLED 2
#endif
#ifndef CLD_DUMPED
#define CLD_DUMPED 3
#endif
#ifndef CLD_TRAPPED
#define CLD_TRAPPED 4
#endif
#ifndef CLD_STOPPED
#define CLD_STOPPED 5
#endif
#ifndef CLD_CONTINUED
#define CLD_CONTINUED 6
#endif

static void k_copy_cstr(char *dst, int dst_len, const char *src)
{
  if(dst == 0 || dst_len <= 0)
    return;
  if(src == 0)
    src = "";
  strncpy(dst, src, (size_t)dst_len - 1u);
  dst[dst_len - 1] = 0;
}

static int k_ptr_byte_readable(const void *ptr)
{
  if(ptr == 0)
    return 0;
  if(esp_ptr_byte_accessible(ptr))
    return 1;
  if(esp_ptr_in_drom(ptr))
    return 1;
  return 0;
}

static int k_ptr_bytes_accessible(const void *ptr, size_t size)
{
  const uint8_t *p = (const uint8_t *)ptr;
  size_t i;

  if(ptr == 0)
    return 0;
  if(size == 0)
    return 1;
  for(i = 0; i < size; i++){
    if(!k_ptr_byte_readable(p + i))
      return 0;
  }
  return 1;
}

static int k_ticks(void)
{
  return (int)hal_ticks();
}

static int k_free_heap(void)
{
  return (int)hal_free_heap_bytes();
}

static int k_total_heap(void)
{
  return (int)hal_total_heap_bytes();
}

static uint32 k_ticks_to_ms_u32(uint32 ticks)
{
  uint64 ms = (uint64)ticks * 10ull;
  if(ms > 0xffffffffull)
    return 0xffffffffu;
  return (uint32)ms;
}

static uint64 k_ticks_to_ms_u64(uint64 ticks)
{
  return ticks * 10ull;
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
extern struct _reent *k_libc_getreent(void) __asm__("__getreent") __attribute__((weak));
extern struct _reent k_picolibc_reent_stub_start __asm__("_picolibc_reent_stub_start") __attribute__((weak));
extern struct _reent k_global_impure_reent __asm__("_global_impure_ptr") __attribute__((weak));

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

static int k_stdio_stream_fd(FILE *f)
{
  if(f == stdin)
    return 0;
  if(f == stdout)
    return 1;
  if(f == stderr)
    return 2;
  return -1;
}

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
  int stdfd;
  int flags;
  int fd;
  ksh_stream_t *s = k_stream_from_file(stream);
  FILE *f;

  path = (const char *)elf_loader_translate_ptr(path);
  mode = (const char *)elf_loader_translate_ptr(mode);
  if(path == 0 || mode == 0){
    errno = EINVAL;
    return 0;
  }
  stdfd = k_stdio_stream_fd(stream);
  if(stdfd >= 0){
    flags = k_stdio_mode_to_flags(mode);
    if(flags < 0)
      return 0;
    fd = k_open(path, flags, 0666);
    if(fd < 0)
      return 0;
    if(fd != stdfd){
      if(k_dup2(fd, stdfd) < 0){
        (void)k_close(fd);
        return 0;
      }
      (void)k_close(fd);
    }
    return stream;
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
  int stdfd = k_stdio_stream_fd(stream);
  unsigned char ch;
  int rc;

  if(stdfd >= 0){
    rc = k_read(stdfd, &ch, 1);
    if(rc == 1)
      return (int)ch;
    return EOF;
  }
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
  char *orig = s;
  int i;
  s = (char *)elf_loader_translate_ptr(s);
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
  return orig;
}

static size_t k_fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
  ksh_stream_t *s = k_stream_from_file(stream);
  int stdfd = k_stdio_stream_fd(stream);
  size_t want;
  int rc;

  ptr = (void *)elf_loader_translate_ptr(ptr);
  if(ptr == 0)
    return 0;
  if(stdfd >= 0){
    if(size == 0 || nmemb == 0)
      return 0;
    want = size * nmemb;
    rc = k_read(stdfd, ptr, want);
    if(rc <= 0)
      return 0;
    return (size_t)rc / size;
  }
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
  int stdfd = k_stdio_stream_fd(stream);
  size_t want;
  int rc;
  ptr = elf_loader_translate_ptr(ptr);
  if(ptr == 0)
    return 0;
  if(stdfd >= 0){
    if(size == 0 || nmemb == 0)
      return 0;
    want = size * nmemb;
    rc = k_write(stdfd, ptr, want);
    if(rc < 0)
      return 0;
    return (size_t)rc / size;
  }
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
  if(stream == 0 || k_stdio_stream_fd(stream) >= 0)
    return 0;
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
  int fd;
  if(stream == stdin)
    return 0;
  if(stream == stdout)
    return 1;
  if(stream == stderr)
    return 2;
  if(s)
    return s->fd;
  fd = fileno(stream);
  return fd;
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

static int k_open(const char *path, int flags, ...)
{
  mode_t mode = 0;
  if(flags & O_CREAT){
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
  return hostabi_posix_fs_open_mode(path, flags, mode);
}

static int k_creat(const char *path, mode_t mode)
{
  return hostabi_posix_fs_creat(path, mode);
}

static int k_read(int fd, void *buf, size_t size)
{
  return hostabi_posix_fs_read(fd, buf, size);
}

static int k_write(int fd, const void *buf, size_t size)
{
  return hostabi_posix_fs_write(fd, buf, size);
}

static int k_close(int fd)
{
  return hostabi_posix_fs_close(fd);
}

static int k_dup(int fd)
{
  return hostabi_posix_fs_dup(fd);
}

static int k_dup2(int oldfd, int newfd)
{
  return hostabi_posix_fs_dup2(oldfd, newfd);
}

static off_t k_lseek(int fd, off_t offset, int whence)
{
  return hostabi_posix_fs_lseek(fd, offset, whence);
}

/*
 * ESP-IDF/lwIP socket APIs require per-task netconn semaphore state.
 * Calls through __xv6_posix_* can happen outside run_elf_command(),
 * so enforce lazy init at the host-ABI boundary.
 */
static void k_posix_socket_ctx_ensure(void)
{
  netconn_thread_init();
}

static int k_posix_close(int fd)
{
  k_posix_socket_ctx_ensure();
  return close(fd);
}

static int k_posix_fcntl(int fd, int cmd, ...)
{
  va_list ap;
  int arg = 0;

  k_posix_socket_ctx_ensure();

  switch(cmd){
#ifdef F_DUPFD
  case F_DUPFD:
#endif
#ifdef F_DUPFD_CLOEXEC
  case F_DUPFD_CLOEXEC:
#endif
#ifdef F_SETFD
  case F_SETFD:
#endif
#ifdef F_SETFL
  case F_SETFL:
#endif
    va_start(ap, cmd);
    arg = va_arg(ap, int);
    va_end(ap);
    return fcntl(fd, cmd, arg);
  default:
    return fcntl(fd, cmd);
  }
}

static int k_posix_ioctl(int fd, unsigned long request, ...)
{
  va_list ap;
  void *arg = 0;

  k_posix_socket_ctx_ensure();

  va_start(ap, request);
  arg = va_arg(ap, void *);
  va_end(ap);

  if(arg)
    return ioctl(fd, request, arg);
  return ioctl(fd, request);
}

static int k_posix_socket(int domain, int type, int protocol)
{
  k_posix_socket_ctx_ensure();
  return socket(domain, type, protocol);
}

static int k_posix_bind(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
  k_posix_socket_ctx_ensure();
  if(addr != 0){
    addr = (const struct sockaddr *)elf_loader_translate_ptr(addr);
    if(addr == 0){
      errno = EINVAL;
      return -1;
    }
  }
  return bind(fd, addr, addrlen);
}

static int k_posix_listen(int fd, int backlog)
{
  k_posix_socket_ctx_ensure();
  return listen(fd, backlog);
}

static int k_posix_accept(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
  k_posix_socket_ctx_ensure();
  if(addr != 0){
    addr = (struct sockaddr *)elf_loader_translate_ptr(addr);
    if(addr == 0){
      errno = EINVAL;
      return -1;
    }
  }
  if(addrlen != 0){
    addrlen = (socklen_t *)elf_loader_translate_ptr(addrlen);
    if(addrlen == 0){
      errno = EINVAL;
      return -1;
    }
  }
  return accept(fd, addr, addrlen);
}

static int k_posix_connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
  k_posix_socket_ctx_ensure();
  if(addr != 0){
    addr = (const struct sockaddr *)elf_loader_translate_ptr(addr);
    if(addr == 0){
      errno = EINVAL;
      return -1;
    }
  }
  return connect(fd, addr, addrlen);
}

static int k_posix_send(int fd, const void *buf, size_t len, int flags)
{
  k_posix_socket_ctx_ensure();
  if(len > 0){
    buf = elf_loader_translate_ptr(buf);
    if(buf == 0){
      errno = EINVAL;
      return -1;
    }
  }
  return (int)send(fd, buf, len, flags);
}

static int k_posix_recv(int fd, void *buf, size_t len, int flags)
{
  k_posix_socket_ctx_ensure();
  if(len > 0){
    buf = (void *)elf_loader_translate_ptr(buf);
    if(buf == 0){
      errno = EINVAL;
      return -1;
    }
  }
  return (int)recv(fd, buf, len, flags);
}

static int k_posix_sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *addr, socklen_t addrlen)
{
  k_posix_socket_ctx_ensure();
  if(len > 0){
    buf = elf_loader_translate_ptr(buf);
    if(buf == 0){
      errno = EINVAL;
      return -1;
    }
  }
  if(addr != 0){
    addr = (const struct sockaddr *)elf_loader_translate_ptr(addr);
    if(addr == 0){
      errno = EINVAL;
      return -1;
    }
  }
  return (int)sendto(fd, buf, len, flags, addr, addrlen);
}

static int k_posix_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *addr, socklen_t *addrlen)
{
  k_posix_socket_ctx_ensure();
  if(len > 0){
    buf = (void *)elf_loader_translate_ptr(buf);
    if(buf == 0){
      errno = EINVAL;
      return -1;
    }
  }
  if(addr != 0){
    addr = (struct sockaddr *)elf_loader_translate_ptr(addr);
    if(addr == 0){
      errno = EINVAL;
      return -1;
    }
  }
  if(addrlen != 0){
    addrlen = (socklen_t *)elf_loader_translate_ptr(addrlen);
    if(addrlen == 0){
      errno = EINVAL;
      return -1;
    }
  }
  return (int)recvfrom(fd, buf, len, flags, addr, addrlen);
}

static int k_posix_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
  fd_set rfds;
  fd_set wfds;
  fd_set efds;
  struct timeval tv;
  struct timeval *tvp = 0;
  int maxfd = -1;
  int ready = 0;
  nfds_t i;
  int rc;

  k_posix_socket_ctx_ensure();

  if(nfds > 0){
    fds = (struct pollfd *)elf_loader_translate_ptr(fds);
    if(fds == 0){
      errno = EINVAL;
      return -1;
    }
  }
  if(timeout < -1){
    errno = EINVAL;
    return -1;
  }

  FD_ZERO(&rfds);
  FD_ZERO(&wfds);
  FD_ZERO(&efds);

  for(i = 0; i < nfds; i++){
    int fd = fds[i].fd;
    short events = fds[i].events;
    fds[i].revents = 0;
    if(fd < 0)
      continue;
    if(fd >= FD_SETSIZE){
      errno = EINVAL;
      return -1;
    }
    if((events & POLLIN) != 0)
      FD_SET(fd, &rfds);
    if((events & POLLOUT) != 0)
      FD_SET(fd, &wfds);
    if((events & (POLLERR | POLLHUP)) != 0)
      FD_SET(fd, &efds);
    if(fd > maxfd)
      maxfd = fd;
  }

  if(timeout >= 0){
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;
    tvp = &tv;
  }

  rc = select(maxfd + 1, &rfds, &wfds, &efds, tvp);
  if(rc <= 0)
    return rc;

  for(i = 0; i < nfds; i++){
    int fd = fds[i].fd;
    short revents = 0;

    if(fd < 0)
      continue;
    if(FD_ISSET(fd, &rfds))
      revents = (short)(revents | POLLIN);
    if(FD_ISSET(fd, &wfds))
      revents = (short)(revents | POLLOUT);
    if(FD_ISSET(fd, &efds))
      revents = (short)(revents | POLLERR);
    fds[i].revents = revents;
    if(revents != 0)
      ready++;
  }

  return ready;
}

static int k_posix_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout)
{
  k_posix_socket_ctx_ensure();
  if(readfds != 0){
    readfds = (fd_set *)elf_loader_translate_ptr(readfds);
    if(readfds == 0){
      errno = EINVAL;
      return -1;
    }
  }
  if(writefds != 0){
    writefds = (fd_set *)elf_loader_translate_ptr(writefds);
    if(writefds == 0){
      errno = EINVAL;
      return -1;
    }
  }
  if(exceptfds != 0){
    exceptfds = (fd_set *)elf_loader_translate_ptr(exceptfds);
    if(exceptfds == 0){
      errno = EINVAL;
      return -1;
    }
  }
  if(timeout != 0){
    timeout = (struct timeval *)elf_loader_translate_ptr(timeout);
    if(timeout == 0){
      errno = EINVAL;
      return -1;
    }
  }
  return select(nfds, readfds, writefds, exceptfds, timeout);
}

static int k_posix_shutdown(int fd, int how)
{
  k_posix_socket_ctx_ensure();
  return shutdown(fd, how);
}

static int k_posix_getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen)
{
  k_posix_socket_ctx_ensure();
  if(optval != 0){
    optval = (void *)elf_loader_translate_ptr(optval);
    if(optval == 0){
      errno = EINVAL;
      return -1;
    }
  }
  if(optlen != 0){
    optlen = (socklen_t *)elf_loader_translate_ptr(optlen);
    if(optlen == 0){
      errno = EINVAL;
      return -1;
    }
  }
  return getsockopt(fd, level, optname, optval, optlen);
}

static int k_posix_setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen)
{
  k_posix_socket_ctx_ensure();
  if(optval != 0){
    optval = elf_loader_translate_ptr(optval);
    if(optval == 0){
      errno = EINVAL;
      return -1;
    }
  }
  return setsockopt(fd, level, optname, optval, optlen);
}

static int k_posix_getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
  k_posix_socket_ctx_ensure();
  if(addr != 0){
    addr = (struct sockaddr *)elf_loader_translate_ptr(addr);
    if(addr == 0){
      errno = EINVAL;
      return -1;
    }
  }
  if(addrlen != 0){
    addrlen = (socklen_t *)elf_loader_translate_ptr(addrlen);
    if(addrlen == 0){
      errno = EINVAL;
      return -1;
    }
  }
  return getsockname(fd, addr, addrlen);
}

static int k_posix_getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
  k_posix_socket_ctx_ensure();
  if(addr != 0){
    addr = (struct sockaddr *)elf_loader_translate_ptr(addr);
    if(addr == 0){
      errno = EINVAL;
      return -1;
    }
  }
  if(addrlen != 0){
    addrlen = (socklen_t *)elf_loader_translate_ptr(addrlen);
    if(addrlen == 0){
      errno = EINVAL;
      return -1;
    }
  }
  return getpeername(fd, addr, addrlen);
}

/*
 * Stable socket ABI stubs.
 * A network kmod can override these symbols at runtime; until then
 * they fail with ENOSYS instead of creating unresolved symbols.
 */
static int k_socket(int domain, int type, int protocol)
{
  (void)domain;
  (void)type;
  (void)protocol;
  errno = ENOSYS;
  return -1;
}

static int k_bind(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
  (void)fd;
  (void)addr;
  (void)addrlen;
  errno = ENOSYS;
  return -1;
}

static int k_listen(int fd, int backlog)
{
  (void)fd;
  (void)backlog;
  errno = ENOSYS;
  return -1;
}

static int k_accept(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
  (void)fd;
  (void)addr;
  (void)addrlen;
  errno = ENOSYS;
  return -1;
}

static int k_connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
  (void)fd;
  (void)addr;
  (void)addrlen;
  errno = ENOSYS;
  return -1;
}

static ssize_t k_send(int fd, const void *buf, size_t len, int flags)
{
  (void)fd;
  (void)buf;
  (void)len;
  (void)flags;
  errno = ENOSYS;
  return -1;
}

static ssize_t k_recv(int fd, void *buf, size_t len, int flags)
{
  (void)fd;
  (void)buf;
  (void)len;
  (void)flags;
  errno = ENOSYS;
  return -1;
}

static ssize_t k_sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *addr, socklen_t addrlen)
{
  (void)fd;
  (void)buf;
  (void)len;
  (void)flags;
  (void)addr;
  (void)addrlen;
  errno = ENOSYS;
  return -1;
}

static ssize_t k_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *addr, socklen_t *addrlen)
{
  (void)fd;
  (void)buf;
  (void)len;
  (void)flags;
  (void)addr;
  (void)addrlen;
  errno = ENOSYS;
  return -1;
}

static int k_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
  (void)fds;
  (void)nfds;
  (void)timeout;
  errno = ENOSYS;
  return -1;
}

static int k_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout)
{
  (void)nfds;
  (void)readfds;
  (void)writefds;
  (void)exceptfds;
  (void)timeout;
  errno = ENOSYS;
  return -1;
}

static int k_shutdown(int fd, int how)
{
  (void)fd;
  (void)how;
  errno = ENOSYS;
  return -1;
}

static int k_getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen)
{
  (void)fd;
  (void)level;
  (void)optname;
  (void)optval;
  (void)optlen;
  errno = ENOSYS;
  return -1;
}

static int k_setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen)
{
  (void)fd;
  (void)level;
  (void)optname;
  (void)optval;
  (void)optlen;
  errno = ENOSYS;
  return -1;
}

static int k_getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
  (void)fd;
  (void)addr;
  (void)addrlen;
  errno = ENOSYS;
  return -1;
}

static int k_getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
  (void)fd;
  (void)addr;
  (void)addrlen;
  errno = ENOSYS;
  return -1;
}

static int k_fstat(int fd, struct stat *st)
{
  return hostabi_posix_fs_fstat(fd, st);
}

static int k_stat(const char *path, struct stat *st)
{
  return hostabi_posix_fs_stat(path, st);
}

static int k_lstat(const char *path, struct stat *st)
{
  return hostabi_posix_fs_lstat(path, st);
}

static int k_access(const char *path, int mode)
{
  path = (const char *)elf_loader_translate_ptr(path);
  if(path == 0){
    errno = EINVAL;
    return -1;
  }
  if(xv6_access(path, mode) != 0){
    errno = xv6_last_errno();
    if(errno <= 0)
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
    errno = xv6_last_errno();
    if(errno <= 0)
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
    errno = xv6_last_errno();
    if(errno <= 0)
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
    errno = xv6_last_errno();
    if(errno <= 0)
      errno = ENOENT;
    return -1;
  }
  return 0;
}

static int k_rmdir(const char *path)
{
  path = (const char *)elf_loader_translate_ptr(path);
  if(path == 0){
    errno = EINVAL;
    return -1;
  }
  if(xv6fs_rmdir_path(path) != 0){
    errno = xv6_last_errno();
    if(errno <= 0)
      errno = EIO;
    return -1;
  }
  return 0;
}

static int k_chdir(const char *path)
{
  path = (const char *)elf_loader_translate_ptr(path);
  if(path == 0){
    errno = EINVAL;
    return -1;
  }
  if(xv6_chdir(path) != 0){
    errno = xv6_last_errno();
    if(errno <= 0)
      errno = ENOENT;
    return -1;
  }
  return 0;
}

static char *k_getcwd(char *buf, size_t size)
{
  char *orig = buf;
  buf = (char *)elf_loader_translate_ptr(buf);
  if(buf == 0 || size == 0){
    errno = EINVAL;
    return 0;
  }
  if(xv6_getcwd(buf, (int)size) != 0){
    errno = xv6_last_errno();
    if(errno <= 0)
      errno = ERANGE;
    return 0;
  }
  return orig;
}

static int k_isatty(int fd)
{
  fd = hostabi_posix_fs_map_fd(fd);
  return hostabi_posix_isatty(fd);
}

static int k_utimes_impl(const char *path, const struct timeval times[2], int follow_final_nonzero)
{
  xv6_kstat_t st;
  const struct timeval *tv = times;

  path = (const char *)elf_loader_translate_ptr(path);
  if(path == 0){
    errno = EINVAL;
    return -1;
  }
  if(times != 0){
    tv = (const struct timeval *)elf_loader_translate_ptr(times);
    if(tv == 0){
      errno = EINVAL;
      return -1;
    }
    if(tv[0].tv_usec < 0 || tv[0].tv_usec > 999999 || tv[1].tv_usec < 0 || tv[1].tv_usec > 999999){
      errno = EINVAL;
      return -1;
    }
  }
  if(follow_final_nonzero){
    if(xv6_stat_path(path, &st) != 0){
      errno = xv6_last_errno();
      if(errno <= 0)
        errno = ENOENT;
      return -1;
    }
  } else {
    if(xv6_lstat_path(path, &st) != 0){
      errno = xv6_last_errno();
      if(errno <= 0)
        errno = ENOENT;
      return -1;
    }
  }
  return 0;
}

static int k_utimes(const char *path, const struct timeval times[2])
{
  return k_utimes_impl(path, times, 1);
}

static int k_lutimes(const char *path, const struct timeval times[2])
{
  return k_utimes_impl(path, times, 0);
}

static mode_t g_umask = 0;

static mode_t k_umask(mode_t mask)
{
  mode_t old = g_umask;
  g_umask = mask;
  return old;
}

static int k_validate_open_fd(int fd)
{
  xv6_kstat_t st;
  fd = hostabi_posix_fs_map_fd(fd);
  if(fd < 0){
    errno = EBADF;
    return -1;
  }
  if(xv6_fstat(fd, &st) != 0){
    errno = EBADF;
    return -1;
  }
  return fd;
}

static int k_fsync(int fd)
{
  if(k_validate_open_fd(fd) < 0)
    return -1;
  return 0;
}

static int k_fdatasync(int fd)
{
  if(k_validate_open_fd(fd) < 0)
    return -1;
  return 0;
}

static void k_sync(void)
{
}

static int k_ftruncate(int fd, off_t length)
{
  int real_fd;
  if(length < 0){
    errno = EINVAL;
    return -1;
  }
  real_fd = k_validate_open_fd(fd);
  if(real_fd < 0)
    return -1;
  if(xv6_ftruncate(real_fd, (long long)length) != 0){
    errno = xv6_last_errno();
    if(errno <= 0)
      errno = EIO;
    return -1;
  }
  return 0;
}

static int k_truncate(const char *path, off_t length)
{
  path = (const char *)elf_loader_translate_ptr(path);
  if(path == 0){
    errno = EINVAL;
    return -1;
  }
  if(length < 0){
    errno = EINVAL;
    return -1;
  }
  if(xv6_truncate_path(path, (long long)length) != 0){
    errno = xv6_last_errno();
    if(errno <= 0)
      errno = EIO;
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
  if(xv6fs_link_path(oldpath, newpath) != 0){
    errno = xv6_last_errno();
    if(errno <= 0)
      errno = EIO;
    return -1;
  }
  return 0;
}

static int k_rename(const char *oldpath, const char *newpath)
{
  oldpath = (const char *)elf_loader_translate_ptr(oldpath);
  newpath = (const char *)elf_loader_translate_ptr(newpath);
  if(oldpath == 0 || newpath == 0){
    errno = EINVAL;
    return -1;
  }
  if(xv6fs_rename_path(oldpath, newpath) != 0){
    errno = xv6_last_errno();
    if(errno <= 0)
      errno = EIO;
    return -1;
  }
  return 0;
}

static int k_symlink(const char *target, const char *linkpath)
{
  target = (const char *)elf_loader_translate_ptr(target);
  linkpath = (const char *)elf_loader_translate_ptr(linkpath);
  if(target == 0 || linkpath == 0){
    errno = EINVAL;
    return -1;
  }
  if(xv6fs_symlink_path(target, linkpath) != 0){
    errno = xv6_last_errno();
    if(errno <= 0)
      errno = EIO;
    return -1;
  }
  return 0;
}

static int k_readlink(const char *path, char *buf, size_t bufsz)
{
  return hostabi_posix_fs_readlink(path, buf, bufsz);
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

#define KSH_SIG_MAX 32
#define KSH_SIG_SLOT_CAP (KSH_MAX_JOBS + 4)

typedef struct {
  TaskHandle_t task;
  void (*handlers[KSH_SIG_MAX])(int);
  uint32 blocked_mask;
  uint32 pending_mask;
} ksh_sig_state_t;

static SemaphoreHandle_t g_sig_state_lock;
static ksh_sig_state_t *g_sig_state;
static int g_sig_state_cap;
static int g_shell_pid = 1;
static int g_shell_sid = 1;
static int g_shell_pgid = 1;
static int g_tty_fg_pgid = 1;

static int k_tty_signal_handler(int sig);
static int terminate_job_id(int id, int exit_code, int reason);
static int suspend_job_id(int id);
static int resume_job_id(int id);

static void sig_state_lock_take(void)
{
  if(g_sig_state_lock == 0)
    g_sig_state_lock = xSemaphoreCreateMutex();
  if(g_sig_state_lock)
    (void)xSemaphoreTake(g_sig_state_lock, portMAX_DELAY);
}

static void sig_state_lock_give(void)
{
  if(g_sig_state_lock)
    (void)xSemaphoreGive(g_sig_state_lock);
}

static int sig_state_ensure_table_locked(void)
{
  if(g_sig_state)
    return 0;
  g_sig_state = (ksh_sig_state_t *)ksh_alloc_data((size_t)KSH_SIG_SLOT_CAP * sizeof(*g_sig_state));
  if(g_sig_state == 0)
    return -1;
  memset(g_sig_state, 0, (size_t)KSH_SIG_SLOT_CAP * sizeof(*g_sig_state));
  g_sig_state_cap = KSH_SIG_SLOT_CAP;
  return 0;
}

static int k_sig_valid(int sig)
{
  return (sig > 0 && sig < KSH_SIG_MAX) ? 1 : 0;
}

static uint32 k_sig_mask_bit(int sig)
{
  if(!k_sig_valid(sig))
    return 0;
  return (uint32)1u << (uint32)(sig - 1);
}

static ksh_sig_state_t *sig_state_slot_locked(TaskHandle_t task, int create)
{
  int i;
  int free_slot = -1;
  if(task == 0)
    return 0;
  if(create && sig_state_ensure_table_locked() != 0)
    return 0;
  for(i = 0; i < g_sig_state_cap; i++){
    if(g_sig_state[i].task == task)
      return &g_sig_state[i];
    if(create && free_slot < 0 && g_sig_state[i].task == 0)
      free_slot = i;
  }
  if(create && free_slot >= 0){
    memset(&g_sig_state[free_slot], 0, sizeof(g_sig_state[free_slot]));
    g_sig_state[free_slot].task = task;
    return &g_sig_state[free_slot];
  }
  return 0;
}

static void k_signal_task_cleanup(TaskHandle_t task)
{
  int i;
  if(task == 0)
    return;
  sig_state_lock_take();
  for(i = 0; i < g_sig_state_cap; i++){
    if(g_sig_state[i].task == task){
      memset(&g_sig_state[i], 0, sizeof(g_sig_state[i]));
      break;
    }
  }
  sig_state_lock_give();
}

static int job_find_slot_by_task_locked(TaskHandle_t task)
{
  int i;
  if(task == 0 || g_jobs == 0)
    return -1;
  for(i = 0; i < KSH_MAX_JOBS; i++){
    if(!g_jobs[i].used)
      continue;
    if(g_jobs[i].task == task)
      return i;
  }
  return -1;
}

static int job_find_slot_by_pid_locked(int pid)
{
  int i;
  if(pid <= 0 || g_jobs == 0)
    return -1;
  for(i = 0; i < KSH_MAX_JOBS; i++){
    if(!g_jobs[i].used)
      continue;
    if(g_jobs[i].pid == pid)
      return i;
  }
  return -1;
}

static int proc_pid_for_task_locked(TaskHandle_t task)
{
  int slot;

  if(task == 0)
    return -1;
  if(g_interactive_task && task == g_interactive_task)
    return g_shell_pid;

  slot = job_find_slot_by_task_locked(task);
  if(slot >= 0)
    return g_jobs[slot].pid;
  return g_shell_pid;
}

static int proc_ppid_for_task_locked(TaskHandle_t task)
{
  int slot;

  if(task == 0)
    return -1;
  if(g_interactive_task && task == g_interactive_task)
    return 0;

  slot = job_find_slot_by_task_locked(task);
  if(slot >= 0)
    return g_jobs[slot].ppid;
  return g_shell_pid;
}

static int proc_pgid_for_task_locked(TaskHandle_t task)
{
  int slot;

  if(task == 0)
    return -1;
  if(g_interactive_task && task == g_interactive_task)
    return g_shell_pgid;

  slot = job_find_slot_by_task_locked(task);
  if(slot >= 0)
    return g_jobs[slot].pgid;
  return g_shell_pgid;
}

static int proc_sid_for_task_locked(TaskHandle_t task)
{
  int slot;

  if(task == 0)
    return -1;
  if(g_interactive_task && task == g_interactive_task)
    return g_shell_sid;

  slot = job_find_slot_by_task_locked(task);
  if(slot >= 0)
    return g_jobs[slot].sid;
  return g_shell_sid;
}

static int proc_group_exists_in_sid_locked(int pgid, int sid)
{
  int i;

  if(pgid <= 0 || sid <= 0)
    return 0;
  if(g_shell_pgid == pgid && g_shell_sid == sid)
    return 1;
  if(g_jobs == 0)
    return 0;
  for(i = 0; i < KSH_MAX_JOBS; i++){
    if(!g_jobs[i].used)
      continue;
    if(g_jobs[i].pgid == pgid && g_jobs[i].sid == sid)
      return 1;
  }
  return 0;
}

static int sig_state_post_pending_locked(TaskHandle_t task, int sig)
{
  ksh_sig_state_t *slot;
  uint32 bit = k_sig_mask_bit(sig);

  if(task == 0 || bit == 0)
    return -1;

  slot = sig_state_slot_locked(task, 1);
  if(slot == 0)
    return -1;

  slot->pending_mask |= bit;
  return 0;
}

static void k_signal_post_pending(TaskHandle_t task, int sig)
{
  sig_state_lock_take();
  (void)sig_state_post_pending_locked(task, sig);
  sig_state_lock_give();
}

static void k_signal_post_sigchld(void)
{
  if(g_interactive_task)
    k_signal_post_pending(g_interactive_task, SIGCHLD);
}

static int k_signal_dispatch_pending_current(void)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();

  while(1){
    ksh_sig_state_t *slot;
    uint32 deliver_mask;
    uint32 bit;
    int sig = 0;
    void (*handler)(int) = SIG_DFL;

    sig_state_lock_take();
    slot = sig_state_slot_locked(self, 0);
    if(slot == 0){
      sig_state_lock_give();
      return 0;
    }

    deliver_mask = slot->pending_mask & ~slot->blocked_mask;
    if(deliver_mask == 0){
      sig_state_lock_give();
      return 0;
    }

    for(sig = 1; sig < KSH_SIG_MAX; sig++){
      bit = k_sig_mask_bit(sig);
      if((deliver_mask & bit) == 0)
        continue;
      slot->pending_mask &= ~bit;
      handler = slot->handlers[sig] ? slot->handlers[sig] : SIG_DFL;
      break;
    }
    sig_state_lock_give();

    if(sig <= 0)
      return 0;

    if(handler == SIG_IGN)
      continue;
    if(handler == SIG_DFL){
      if(sig == SIGCHLD)
        continue;
      k_exit(128 + sig);
      return 0;
    }
    handler(sig);
  }
}

static void (*k_signal(int sig, void (*handler)(int)))(int)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  ksh_sig_state_t *slot;
  void (*old_handler)(int);

  if(!k_sig_valid(sig) || handler == SIG_ERR){
    errno = EINVAL;
    return SIG_ERR;
  }

  sig_state_lock_take();
  slot = sig_state_slot_locked(self, 1);
  if(slot == 0){
    sig_state_lock_give();
    errno = ENOMEM;
    return SIG_ERR;
  }

  old_handler = slot->handlers[sig];
  slot->handlers[sig] = handler;
  sig_state_lock_give();
  return old_handler ? old_handler : SIG_DFL;
}

static int k_sigaction(int sig, const struct sigaction *act, struct sigaction *oldact)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  ksh_sig_state_t *slot;
  void (*old_handler)(int);

  act = (const struct sigaction *)elf_loader_translate_ptr(act);
  oldact = (struct sigaction *)elf_loader_translate_ptr(oldact);

  if(!k_sig_valid(sig)){
    errno = EINVAL;
    return -1;
  }
  if(act && !k_ptr_bytes_accessible(act, sizeof(*act))){
    errno = EINVAL;
    return -1;
  }
  if(oldact && !k_ptr_bytes_accessible(oldact, sizeof(*oldact))){
    errno = EINVAL;
    return -1;
  }

  sig_state_lock_take();
  slot = sig_state_slot_locked(self, (act != 0 || oldact != 0) ? 1 : 0);
  if((act || oldact) && slot == 0){
    sig_state_lock_give();
    errno = ENOMEM;
    return -1;
  }

  old_handler = (slot && slot->handlers[sig]) ? slot->handlers[sig] : SIG_DFL;
  if(oldact){
    memset(oldact, 0, sizeof(*oldact));
    oldact->sa_handler = old_handler;
    memset(&oldact->sa_mask, 0, sizeof(oldact->sa_mask));
  }
  if(act){
    if(act->sa_handler == SIG_ERR){
      sig_state_lock_give();
      errno = EINVAL;
      return -1;
    }
    slot->handlers[sig] = act->sa_handler;
  }
  sig_state_lock_give();
  return 0;
}

static int k_sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  ksh_sig_state_t *slot;
  const sigset_t *set_host;
  sigset_t *oldset_host;
  uint32 new_mask;
  uint32 old_mask = 0;

  set_host = (const sigset_t *)elf_loader_translate_ptr(set);
  oldset_host = (sigset_t *)elf_loader_translate_ptr(oldset);
  if(set && (set_host == 0 || !k_ptr_bytes_accessible(set_host, sizeof(*set_host)))){
    errno = EINVAL;
    return -1;
  }
  if(oldset && (oldset_host == 0 || !k_ptr_bytes_accessible(oldset_host, sizeof(*oldset_host)))){
    errno = EINVAL;
    return -1;
  }

  sig_state_lock_take();
  slot = sig_state_slot_locked(self, 1);
  if(slot == 0){
    sig_state_lock_give();
    errno = ENOMEM;
    return -1;
  }

  old_mask = slot->blocked_mask;
  if(set_host){
    memcpy(&new_mask, set_host, sizeof(new_mask));
    switch(how){
    case SIG_BLOCK:
      slot->blocked_mask |= new_mask;
      break;
    case SIG_UNBLOCK:
      slot->blocked_mask &= ~new_mask;
      break;
    case SIG_SETMASK:
      slot->blocked_mask = new_mask;
      break;
    default:
      sig_state_lock_give();
      errno = EINVAL;
      return -1;
    }
  }
  if(oldset_host){
    memset(oldset_host, 0, sizeof(*oldset_host));
    memcpy(oldset_host, &old_mask, sizeof(old_mask));
  }
  sig_state_lock_give();
  (void)k_signal_dispatch_pending_current();
  return 0;
}

static int k_sigemptyset(sigset_t *set)
{
  set = (sigset_t *)elf_loader_translate_ptr(set);
  if(set == 0){
    errno = EINVAL;
    return -1;
  }
  if(!k_ptr_bytes_accessible(set, sizeof(*set))){
    errno = EINVAL;
    return -1;
  }
  memset(set, 0, sizeof(*set));
  return 0;
}

static int k_sigfillset(sigset_t *set)
{
  set = (sigset_t *)elf_loader_translate_ptr(set);
  if(set == 0){
    errno = EINVAL;
    return -1;
  }
  if(!k_ptr_bytes_accessible(set, sizeof(*set))){
    errno = EINVAL;
    return -1;
  }
  memset(set, 0xff, sizeof(*set));
  return 0;
}

static int k_sigaddset(sigset_t *set, int signo)
{
  uint32 mask;

  set = (sigset_t *)elf_loader_translate_ptr(set);
  if(set == 0){
    errno = EINVAL;
    return -1;
  }
  if(!k_ptr_bytes_accessible(set, sizeof(*set))){
    errno = EINVAL;
    return -1;
  }
  if(!k_sig_valid(signo)){
    errno = EINVAL;
    return -1;
  }
  memcpy(&mask, set, sizeof(mask));
  mask |= k_sig_mask_bit(signo);
  memcpy(set, &mask, sizeof(mask));
  return 0;
}

static int k_sigdelset(sigset_t *set, int signo)
{
  uint32 mask;

  set = (sigset_t *)elf_loader_translate_ptr(set);
  if(set == 0){
    errno = EINVAL;
    return -1;
  }
  if(!k_ptr_bytes_accessible(set, sizeof(*set))){
    errno = EINVAL;
    return -1;
  }
  if(!k_sig_valid(signo)){
    errno = EINVAL;
    return -1;
  }
  memcpy(&mask, set, sizeof(mask));
  mask &= ~k_sig_mask_bit(signo);
  memcpy(set, &mask, sizeof(mask));
  return 0;
}

static int k_sigismember(const sigset_t *set, int signo)
{
  uint32 mask;

  set = (const sigset_t *)elf_loader_translate_ptr(set);
  if(set == 0 || !k_sig_valid(signo)){
    errno = EINVAL;
    return -1;
  }
  if(!k_ptr_bytes_accessible(set, sizeof(*set))){
    errno = EINVAL;
    return -1;
  }
  memcpy(&mask, set, sizeof(mask));
  return (mask & k_sig_mask_bit(signo)) ? 1 : 0;
}

static int k_raise(int sig)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  ksh_sig_state_t *slot;
  void (*handler)(int) = SIG_DFL;
  uint32 bit;

  if(!k_sig_valid(sig)){
    errno = EINVAL;
    return -1;
  }
  bit = k_sig_mask_bit(sig);

  sig_state_lock_take();
  slot = sig_state_slot_locked(self, 1);
  if(slot == 0){
    sig_state_lock_give();
    errno = ENOMEM;
    return -1;
  }
  if(slot && slot->handlers[sig])
    handler = slot->handlers[sig];
  if(slot && (slot->blocked_mask & bit) != 0){
    slot->pending_mask |= bit;
    sig_state_lock_give();
    return 0;
  }
  sig_state_lock_give();

  if(handler == SIG_IGN)
    return 0;
  if(handler == SIG_DFL){
    if(sig == SIGCHLD)
      return 0;
    k_exit(128 + sig);
    return 0;
  }
  handler(sig);
  (void)k_signal_dispatch_pending_current();
  return 0;
}

static int k_signal_job_pid(int pid, int sig)
{
  int slot = -1;
  int job_id = -1;

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  slot = job_find_slot_by_pid_locked(pid);
  if(slot >= 0 && g_jobs[slot].used)
    job_id = g_jobs[slot].id;
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(job_id <= 0){
    errno = ESRCH;
    return -1;
  }
  if(sig == 0)
    return 0;
  if(sig == SIGCONT)
    return resume_job_id(job_id);
  if(sig == SIGTSTP)
    return suspend_job_id(job_id);
  if(sig == SIGCHLD)
    return 0;
  return terminate_job_id(job_id, 128 + sig, JOB_REASON_KILLED);
}

static int k_signal_group_jobs(int pgid, int sig, int *out_any)
{
  int i;
  int any = 0;
  int pids[KSH_MAX_JOBS];
  int npids = 0;

  if(pgid <= 0){
    errno = EINVAL;
    return -1;
  }

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  if(g_jobs){
    for(i = 0; i < KSH_MAX_JOBS; i++){
      if(!g_jobs[i].used)
        continue;
      if(g_jobs[i].pgid != pgid)
        continue;
      if(npids < KSH_MAX_JOBS)
        pids[npids++] = g_jobs[i].pid;
    }
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  for(i = 0; i < npids; i++){
    if(k_signal_job_pid(pids[i], sig) == 0)
      any = 1;
  }
  if(out_any)
    *out_any = any;
  if(any)
    return 0;
  errno = ESRCH;
  return -1;
}

static int k_kill(int pid, int sig)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  int self_pid;
  int pgid;
  int any = 0;

  if(sig < 0 || sig >= KSH_SIG_MAX){
    errno = EINVAL;
    return -1;
  }

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  self_pid = proc_pid_for_task_locked(self);
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(pid > 0){
    if(pid == g_shell_pid){
      if(sig == 0)
        return 0;
      if(self_pid == g_shell_pid)
        return k_raise(sig);
      errno = EPERM;
      return -1;
    }
    return k_signal_job_pid(pid, sig);
  }

  if(pid == 0){
    if(g_jobs_lock)
      (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
    pgid = proc_pgid_for_task_locked(self);
    if(g_jobs_lock)
      (void)xSemaphoreGive(g_jobs_lock);
    if(pgid <= 0){
      errno = ESRCH;
      return -1;
    }
    if(pgid == g_shell_pgid){
      if(self_pid == g_shell_pid && sig != 0){
        (void)k_raise(sig);
        any = 1;
      } else if(sig == 0){
        any = 1;
      }
    }
    if(k_signal_group_jobs(pgid, sig, &any) == 0 || any)
      return 0;
    return -1;
  }

  if(pid == -1){
    errno = EPERM;
    return -1;
  }

  pgid = -pid;
  if(pgid == g_shell_pgid){
    if(self_pid == g_shell_pid && sig != 0){
      (void)k_raise(sig);
      any = 1;
    } else if(sig == 0){
      any = 1;
    }
  }
  if(k_signal_group_jobs(pgid, sig, &any) == 0 || any)
    return 0;
  return -1;
}

static int k_tty_signal_handler(int sig)
{
  int fg_pgid;

  if(sig != SIGINT && sig != SIGTSTP){
    errno = EINVAL;
    return -1;
  }

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  fg_pgid = g_tty_fg_pgid;
  if(fg_pgid <= 0)
    fg_pgid = g_shell_pgid;
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(fg_pgid <= 0){
    errno = ESRCH;
    return -1;
  }
  return k_kill(-fg_pgid, sig);
}

enum {
  K_WAIT_EVENT_NONE = 0,
  K_WAIT_EVENT_EXITED,
  K_WAIT_EVENT_STOPPED,
  K_WAIT_EVENT_CONTINUED,
};

static int k_wait_encode_status_locked(const ksh_job_t *job, int event)
{
  int sig;

  if(job == 0)
    return 0;
  switch(event){
  case K_WAIT_EVENT_EXITED:
    if(job->reason == JOB_REASON_KILLED){
      sig = 0;
      if(job->exit_code >= 128 && job->exit_code < (128 + KSH_SIG_MAX))
        sig = job->exit_code - 128;
      if(sig <= 0 || sig >= KSH_SIG_MAX)
        sig = SIGKILL;
      return sig & 0x7f;
    }
    return (job->exit_code & 0xff) << 8;
  case K_WAIT_EVENT_STOPPED:
    return ((SIGTSTP & 0xff) << 8) | 0x7f;
  case K_WAIT_EVENT_CONTINUED:
    return 0xffff;
  default:
    return 0;
  }
}

static int k_waitpid_match_child_locked(const ksh_job_t *job, int self_pid, int self_pgid, pid_t pid)
{
  if(job == 0 || !job->used)
    return 0;
  if(job->ppid != self_pid)
    return 0;
  if(pid > 0)
    return job->pid == (int)pid;
  if(pid == 0)
    return job->pgid == self_pgid;
  if(pid == (pid_t)-1)
    return 1;
  if(pid < (pid_t)-1)
    return job->pgid == (int)(-pid);
  return 0;
}

static int k_waitid_match_child_locked(const ksh_job_t *job, int self_pid, int self_pgid, int idtype, int id)
{
  if(job == 0 || !job->used)
    return 0;
  if(job->ppid != self_pid)
    return 0;
  switch(idtype){
  case P_ALL:
    return 1;
  case P_PID:
    return job->pid == id;
  case P_PGID:
    if(id == 0)
      return job->pgid == self_pgid;
    return job->pgid == id;
  default:
    return 0;
  }
}

static int k_wait_pick_event_locked(const ksh_job_t *job, int want_exit, int want_stop, int want_cont)
{
  if(job == 0 || !job->used)
    return K_WAIT_EVENT_NONE;
  if(want_exit && job->done && job->wait_pending_exit)
    return K_WAIT_EVENT_EXITED;
  if(want_stop && !job->done && job->wait_pending_stop)
    return K_WAIT_EVENT_STOPPED;
  if(want_cont && !job->done && !job->stopped && job->wait_pending_cont)
    return K_WAIT_EVENT_CONTINUED;
  return K_WAIT_EVENT_NONE;
}

static void k_wait_fill_siginfo(siginfo_t *si, int pid, int event, int status)
{
  if(si == 0)
    return;
  memset(si, 0, sizeof(*si));
  si->si_signo = SIGCHLD;
  si->si_pid = (pid_t)pid;
  switch(event){
  case K_WAIT_EVENT_STOPPED:
    si->si_code = CLD_STOPPED;
    si->si_status = (status >> 8) & 0xff;
    break;
  case K_WAIT_EVENT_CONTINUED:
    si->si_code = CLD_CONTINUED;
    si->si_status = SIGCONT;
    break;
  case K_WAIT_EVENT_EXITED:
  default:
    if((status & 0x7f) == 0){
      si->si_code = CLD_EXITED;
      si->si_status = (status >> 8) & 0xff;
    } else {
      si->si_code = CLD_KILLED;
      si->si_status = status & 0x7f;
    }
    break;
  }
}

static int k_wait_consume_event_locked(int slot, int event, int nowait)
{
  if(slot < 0 || slot >= KSH_MAX_JOBS || g_jobs == 0 || !g_jobs[slot].used)
    return -1;
  if(nowait)
    return 0;

  switch(event){
  case K_WAIT_EVENT_EXITED:
    memset(&g_jobs[slot], 0, sizeof(g_jobs[slot]));
    return 0;
  case K_WAIT_EVENT_STOPPED:
    g_jobs[slot].wait_pending_stop = 0;
    return 0;
  case K_WAIT_EVENT_CONTINUED:
    g_jobs[slot].wait_pending_cont = 0;
    return 0;
  default:
    return -1;
  }
}

static pid_t k_waitpid(pid_t pid, int *status, int options)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  int *status_host;
  int self_pid;
  int self_pgid;

  status_host = (int *)elf_loader_translate_ptr(status);
  if(status && (status_host == 0 || !k_ptr_bytes_accessible(status_host, sizeof(*status_host)))){
    errno = EINVAL;
    return (pid_t)-1;
  }
  if((options & ~(WNOHANG | WUNTRACED | WCONTINUED)) != 0){
    errno = EINVAL;
    return (pid_t)-1;
  }

  while(1){
    int i;
    int has_child = 0;
    int slot = -1;
    int event = K_WAIT_EVENT_NONE;
    int encoded_status = 0;
    int child_pid = -1;

    if(g_jobs_lock)
      (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
    self_pid = proc_pid_for_task_locked(self);
    self_pgid = proc_pgid_for_task_locked(self);
    if(self_pid <= 0)
      self_pid = g_shell_pid;
    if(self_pgid <= 0)
      self_pgid = g_shell_pgid;

    if(g_jobs){
      for(i = 0; i < KSH_MAX_JOBS; i++){
        if(!k_waitpid_match_child_locked(&g_jobs[i], self_pid, self_pgid, pid))
          continue;
        has_child = 1;
        event = k_wait_pick_event_locked(&g_jobs[i], 1, (options & WUNTRACED) != 0, (options & WCONTINUED) != 0);
        if(event == K_WAIT_EVENT_NONE)
          continue;
        slot = i;
        child_pid = g_jobs[i].pid;
        encoded_status = k_wait_encode_status_locked(&g_jobs[i], event);
        (void)k_wait_consume_event_locked(slot, event, 0);
        break;
      }
    }
    if(g_jobs_lock)
      (void)xSemaphoreGive(g_jobs_lock);

    if(slot >= 0){
      if(status_host)
        *status_host = encoded_status;
      return (pid_t)child_pid;
    }
    if(!has_child){
      errno = ECHILD;
      return (pid_t)-1;
    }
    if(options & WNOHANG)
      return (pid_t)0;

    (void)k_signal_dispatch_pending_current();
    hal_delay_ms(10);
  }
}

static pid_t k_wait(int *status)
{
  return k_waitpid((pid_t)-1, status, 0);
}

static int k_waitid(int idtype, int id, siginfo_t *infop, int options)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  siginfo_t *si;
  int self_pid;
  int self_pgid;
  int want_exit;
  int want_stop;
  int want_cont;
  int nowait;

  si = (siginfo_t *)elf_loader_translate_ptr(infop);
  if(si == 0 || !k_ptr_bytes_accessible(si, sizeof(*si))){
    errno = EINVAL;
    return -1;
  }

  if((options & ~(WNOHANG | WNOWAIT | WEXITED | WSTOPPED | WCONTINUED)) != 0){
    errno = EINVAL;
    return -1;
  }

  want_exit = (options & WEXITED) != 0;
  want_stop = (options & WSTOPPED) != 0;
  want_cont = (options & WCONTINUED) != 0;
  if(!want_exit && !want_stop && !want_cont){
    errno = EINVAL;
    return -1;
  }

  if(idtype != P_ALL && idtype != P_PID && idtype != P_PGID){
    errno = EINVAL;
    return -1;
  }
  nowait = (options & WNOWAIT) ? 1 : 0;

  while(1){
    int i;
    int has_child = 0;
    int slot = -1;
    int event = K_WAIT_EVENT_NONE;
    int encoded_status = 0;
    int child_pid = -1;

    if(g_jobs_lock)
      (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
    self_pid = proc_pid_for_task_locked(self);
    self_pgid = proc_pgid_for_task_locked(self);
    if(self_pid <= 0)
      self_pid = g_shell_pid;
    if(self_pgid <= 0)
      self_pgid = g_shell_pgid;

    if(g_jobs){
      for(i = 0; i < KSH_MAX_JOBS; i++){
        if(!k_waitid_match_child_locked(&g_jobs[i], self_pid, self_pgid, idtype, id))
          continue;
        has_child = 1;
        event = k_wait_pick_event_locked(&g_jobs[i], want_exit, want_stop, want_cont);
        if(event == K_WAIT_EVENT_NONE)
          continue;
        slot = i;
        child_pid = g_jobs[i].pid;
        encoded_status = k_wait_encode_status_locked(&g_jobs[i], event);
        (void)k_wait_consume_event_locked(slot, event, nowait);
        break;
      }
    }
    if(g_jobs_lock)
      (void)xSemaphoreGive(g_jobs_lock);

    if(slot >= 0){
      k_wait_fill_siginfo(si, child_pid, event, encoded_status);
      return 0;
    }
    if(!has_child){
      errno = ECHILD;
      return -1;
    }
    if(options & WNOHANG){
      memset(si, 0, sizeof(*si));
      return 0;
    }

    (void)k_signal_dispatch_pending_current();
    hal_delay_ms(10);
  }
}

static pid_t k_getpid(void)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  int pid;

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  pid = proc_pid_for_task_locked(self);
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);
  if(pid <= 0)
    pid = g_shell_pid;
  return (pid_t)pid;
}

static pid_t k_getppid(void)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  int ppid;

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  ppid = proc_ppid_for_task_locked(self);
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);
  if(ppid < 0)
    ppid = 0;
  return (pid_t)ppid;
}

static pid_t k_getpgid(pid_t pid)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  int target = (int)pid;
  int pgid = -1;
  int slot = -1;

  if(target == 0){
    if(g_jobs_lock)
      (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
    pgid = proc_pgid_for_task_locked(self);
    if(g_jobs_lock)
      (void)xSemaphoreGive(g_jobs_lock);
    if(pgid <= 0)
      pgid = g_shell_pgid;
    return (pid_t)pgid;
  }
  if(target < 0){
    errno = EINVAL;
    return (pid_t)-1;
  }

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  if(target == g_shell_pid)
    pgid = g_shell_pgid;
  else {
    slot = job_find_slot_by_pid_locked(target);
    if(slot >= 0 && g_jobs[slot].used)
      pgid = g_jobs[slot].pgid;
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(pgid <= 0){
    errno = ESRCH;
    return (pid_t)-1;
  }
  return (pid_t)pgid;
}

static pid_t k_getpgrp(void)
{
  return k_getpgid((pid_t)0);
}

static int k_setpgid(pid_t pid, pid_t pgid)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  int self_pid;
  int target;
  int new_pgid;
  int sid;
  int slot = -1;
  int rc = -1;

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);

  self_pid = proc_pid_for_task_locked(self);
  target = (pid == 0) ? self_pid : (int)pid;
  if(target <= 0){
    errno = EINVAL;
    goto out;
  }

  new_pgid = (pgid == 0) ? target : (int)pgid;
  if(new_pgid <= 0){
    errno = EINVAL;
    goto out;
  }

  if(target == g_shell_pid){
    if(self_pid != g_shell_pid){
      errno = EPERM;
      goto out;
    }
    sid = g_shell_sid;
    if(new_pgid != target && !proc_group_exists_in_sid_locked(new_pgid, sid)){
      errno = EPERM;
      goto out;
    }
    g_shell_pgid = new_pgid;
    rc = 0;
    goto out;
  }

  slot = job_find_slot_by_pid_locked(target);
  if(slot < 0 || !g_jobs[slot].used){
    errno = ESRCH;
    goto out;
  }
  if(self_pid != g_shell_pid && self_pid != target){
    errno = EPERM;
    goto out;
  }

  sid = g_jobs[slot].sid;
  if(sid <= 0)
    sid = target;
  if(new_pgid != target && !proc_group_exists_in_sid_locked(new_pgid, sid)){
    errno = EPERM;
    goto out;
  }

  g_jobs[slot].pgid = new_pgid;
  rc = 0;

out:
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);
  return rc;
}

static pid_t k_setsid(void)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  int self_pid;
  int self_pgid;
  int slot = -1;
  int sid = -1;

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);

  self_pid = proc_pid_for_task_locked(self);
  self_pgid = proc_pgid_for_task_locked(self);
  if(self_pid <= 0){
    errno = ESRCH;
    goto out;
  }
  if(self_pid == self_pgid){
    errno = EPERM;
    goto out;
  }

  if(self_pid == g_shell_pid){
    g_shell_sid = self_pid;
    g_shell_pgid = self_pid;
    sid = g_shell_sid;
    goto out;
  }

  slot = job_find_slot_by_pid_locked(self_pid);
  if(slot < 0 || !g_jobs[slot].used){
    errno = ESRCH;
    goto out;
  }
  g_jobs[slot].sid = self_pid;
  g_jobs[slot].pgid = self_pid;
  sid = g_jobs[slot].sid;

out:
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);
  return (sid > 0) ? (pid_t)sid : (pid_t)-1;
}

static pid_t k_getsid(pid_t pid)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  int target = (int)pid;
  int sid = -1;
  int slot = -1;

  if(target == 0){
    if(g_jobs_lock)
      (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
    sid = proc_sid_for_task_locked(self);
    if(g_jobs_lock)
      (void)xSemaphoreGive(g_jobs_lock);
    if(sid <= 0)
      sid = g_shell_sid;
    return (pid_t)sid;
  }
  if(target < 0){
    errno = EINVAL;
    return (pid_t)-1;
  }

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  if(target == g_shell_pid)
    sid = g_shell_sid;
  else {
    slot = job_find_slot_by_pid_locked(target);
    if(slot >= 0 && g_jobs[slot].used)
      sid = g_jobs[slot].sid;
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(sid <= 0){
    errno = ESRCH;
    return (pid_t)-1;
  }
  return (pid_t)sid;
}

static int k_tcgetpgrp(int fd)
{
  if(hostabi_posix_isatty(fd) == 0){
    if(errno == 0)
      errno = ENOTTY;
    return -1;
  }

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  fd = g_tty_fg_pgid;
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(fd <= 0){
    errno = ESRCH;
    return -1;
  }
  return fd;
}

static int k_tcsetpgrp(int fd, pid_t pgrp)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  int sid;
  int target_pgrp = (int)pgrp;

  if(hostabi_posix_isatty(fd) == 0){
    if(errno == 0)
      errno = ENOTTY;
    return -1;
  }
  if(target_pgrp <= 0){
    errno = EINVAL;
    return -1;
  }

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  sid = proc_sid_for_task_locked(self);
  if(sid <= 0)
    sid = g_shell_sid;
  if(!proc_group_exists_in_sid_locked(target_pgrp, sid)){
    if(g_jobs_lock)
      (void)xSemaphoreGive(g_jobs_lock);
    errno = EPERM;
    return -1;
  }
  g_tty_fg_pgid = target_pgrp;
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);
  return 0;
}

static unsigned int k_sleep(unsigned int sec)
{
  usleep(sec * 1000000U);
  return 0;
}

static int k_fchmod(int fd, mode_t mode)
{
  fd = k_validate_open_fd(fd);
  if(fd < 0)
    return -1;
  if(xv6_fchmod(fd, (int)mode) != 0){
    errno = xv6_last_errno();
    if(errno <= 0)
      errno = EIO;
    return -1;
  }
  return 0;
}

static int k_chown(const char *path, uid_t owner, gid_t group)
{
  int owner_i = (owner == (uid_t)-1) ? -1 : (int)owner;
  int group_i = (group == (gid_t)-1) ? -1 : (int)group;

  path = (const char *)elf_loader_translate_ptr(path);
  if(path == 0){
    errno = EINVAL;
    return -1;
  }
  if(xv6_chown_path(path, owner_i, group_i, 1) != 0){
    errno = xv6_last_errno();
    if(errno <= 0)
      errno = EIO;
    return -1;
  }
  return 0;
}

static int k_lchown(const char *path, uid_t owner, gid_t group)
{
  int owner_i = (owner == (uid_t)-1) ? -1 : (int)owner;
  int group_i = (group == (gid_t)-1) ? -1 : (int)group;

  path = (const char *)elf_loader_translate_ptr(path);
  if(path == 0){
    errno = EINVAL;
    return -1;
  }
  if(xv6_chown_path(path, owner_i, group_i, 0) != 0){
    errno = xv6_last_errno();
    if(errno <= 0)
      errno = EIO;
    return -1;
  }
  return 0;
}

static int k_fchown(int fd, uid_t owner, gid_t group)
{
  int owner_i = (owner == (uid_t)-1) ? -1 : (int)owner;
  int group_i = (group == (gid_t)-1) ? -1 : (int)group;

  fd = k_validate_open_fd(fd);
  if(fd < 0)
    return -1;
  if(xv6_fchown(fd, owner_i, group_i) != 0){
    errno = xv6_last_errno();
    if(errno <= 0)
      errno = EIO;
    return -1;
  }
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

  if(arg[1] == '-' && arg[2] == '\0'){
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

static void k_reent_set_errno(struct _reent *r, int err)
{
  if(err <= 0)
    err = EIO;
  errno = err;
  #if CONFIG_LIBC_NEWLIB
  if(r)
    r->_errno = err;
  #else
  (void)r;
  #endif
}

static void k_reent_clear_errno(struct _reent *r)
{
  errno = 0;
  #if CONFIG_LIBC_NEWLIB
  if(r)
    r->_errno = 0;
  #else
  (void)r;
  #endif
}

static struct _reent *k_getreent(void)
{
  #if CONFIG_LIBC_NEWLIB
  return __getreent();
  #else
  static struct _reent fallback_reent;
  struct _reent *r = 0;

  if(k_libc_getreent)
    r = k_libc_getreent();
  if(r)
    return r;
  if((uintptr_t)&k_picolibc_reent_stub_start != 0)
    return &k_picolibc_reent_stub_start;
  if((uintptr_t)&k_global_impure_reent != 0)
    return &k_global_impure_reent;
  return &fallback_reent;
  #endif
}

static int k_reent_finish_int(struct _reent *r, int rc)
{
  if(rc < 0){
    k_reent_set_errno(r, errno);
    return -1;
  }
  k_reent_clear_errno(r);
  return rc;
}

static _ssize_t k_reent_finish_ssize(struct _reent *r, int rc)
{
  if(rc < 0){
    k_reent_set_errno(r, errno);
    return (_ssize_t)-1;
  }
  k_reent_clear_errno(r);
  return (_ssize_t)rc;
}

static _off_t k__lseek_r(struct _reent *r, int fd, _off_t off, int whence)
{
  off_t rc = k_lseek(fd, (off_t)off, whence);
  if(rc < 0){
    k_reent_set_errno(r, errno);
    return (_off_t)-1;
  }
  k_reent_clear_errno(r);
  return (_off_t)rc;
}

static _ssize_t k__read_r(struct _reent *r, int fd, void *buf, size_t cnt)
{
  return k_reent_finish_ssize(r, k_read(fd, buf, cnt));
}

static _ssize_t k__write_r(struct _reent *r, int fd, const void *buf, size_t cnt)
{
  return k_reent_finish_ssize(r, k_write(fd, buf, cnt));
}

static int k__close_r(struct _reent *r, int fd)
{
  return k_reent_finish_int(r, k_close(fd));
}

static int k__open_r(struct _reent *r, const char *path, int flags, int mode)
{
  return k_reent_finish_int(r, k_open(path, flags, mode));
}

static int k__fstat_r(struct _reent *r, int fd, struct stat *st)
{
  return k_reent_finish_int(r, k_fstat(fd, st));
}

static int k__stat_r(struct _reent *r, const char *path, struct stat *st)
{
  return k_reent_finish_int(r, k_stat(path, st));
}

static int k__lstat_r(struct _reent *r, const char *path, struct stat *st)
{
  return k_reent_finish_int(r, k_lstat(path, st));
}

static int k__isatty_r(struct _reent *r, int fd)
{
  return k_reent_finish_int(r, k_isatty(fd));
}

static int k__unlink_r(struct _reent *r, const char *path)
{
  return k_reent_finish_int(r, k_unlink(path));
}

static int k__mkdir_r(struct _reent *r, const char *path, int mode)
{
  return k_reent_finish_int(r, k_mkdir(path, (mode_t)mode));
}

static int k__rmdir_r(struct _reent *r, const char *path)
{
  return k_reent_finish_int(r, k_rmdir(path));
}

static int k__rename_r(struct _reent *r, const char *oldpath, const char *newpath)
{
  return k_reent_finish_int(r, k_rename(oldpath, newpath));
}

static int k__link_r(struct _reent *r, const char *oldpath, const char *newpath)
{
  return k_reent_finish_int(r, k_link(oldpath, newpath));
}

static int k__gettimeofday_r(struct _reent *r, struct timeval *tp, void *tzp)
{
  uint32 ms;
  struct timeval *tp_host;
  struct timezone *tz = (struct timezone *)elf_loader_translate_ptr(tzp);

  /* cppcheck-suppress uninitvar */
  tp_host = (struct timeval *)elf_loader_translate_ptr(tp);

  if(tp_host == 0 || !k_ptr_bytes_accessible(tp_host, sizeof(*tp_host))){
    k_reent_set_errno(r, EINVAL);
    return -1;
  }
  ms = hal_ticks();
  tp_host->tv_sec = (time_t)(ms / 1000U);
  tp_host->tv_usec = (suseconds_t)((ms % 1000U) * 1000U);
  if(tz && k_ptr_bytes_accessible(tz, sizeof(*tz))){
    tz->tz_minuteswest = 0;
    tz->tz_dsttime = 0;
  }
  k_reent_clear_errno(r);
  return 0;
}

static clock_t k__times_r(struct _reent *r, struct tms *buf)
{
  clock_t now = (clock_t)hal_ticks();

  buf = (struct tms *)elf_loader_translate_ptr(buf);
  if(buf){
    if(!k_ptr_bytes_accessible(buf, sizeof(*buf))){
      k_reent_set_errno(r, EINVAL);
      return (clock_t)-1;
    }
    buf->tms_utime = now;
    buf->tms_stime = 0;
    buf->tms_cutime = 0;
    buf->tms_cstime = 0;
  }
  k_reent_clear_errno(r);
  return now;
}

static int k__kill_r(struct _reent *r, int pid, int sig)
{
  return k_reent_finish_int(r, k_kill(pid, sig));
}

static int k__wait_r(struct _reent *r, int *status)
{
  pid_t rc = k_wait(status);
  if(rc < 0){
    k_reent_set_errno(r, errno);
    return -1;
  }
  k_reent_clear_errno(r);
  return (int)rc;
}

static int k__waitpid_r(struct _reent *r, int pid, int *status, int options)
{
  pid_t rc = k_waitpid((pid_t)pid, status, options);
  if(rc < 0){
    k_reent_set_errno(r, errno);
    return -1;
  }
  k_reent_clear_errno(r);
  return (int)rc;
}

static int k__getpid_r(struct _reent *r)
{
  k_reent_clear_errno(r);
  return (int)k_getpid();
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
    k_reent_set_errno(r, ENOMEM);
    return (caddr_t)-1;
  }
  old = used;
  used += (size_t)incr;
  k_reent_clear_errno(r);
  return (caddr_t)(arena + old);
}

static caddr_t k_sbrk(ptrdiff_t incr)
{
  return k__sbrk_r(k_getreent(), incr);
}

static caddr_t k__sbrk(ptrdiff_t incr)
{
  return k__sbrk_r(k_getreent(), incr);
}

static int k_gettimeofday(struct timeval *tp, void *tzp)
{
  return k__gettimeofday_r(k_getreent(), tp, tzp);
}

static clock_t k_times(struct tms *buf)
{
  return k__times_r(k_getreent(), buf);
}

static time_t k_time(time_t *out)
{
  struct timeval tv;
  if(k_gettimeofday(&tv, 0) != 0)
    return (time_t)-1;
  if(out)
    *out = tv.tv_sec;
  return tv.tv_sec;
}

static void k_exit(int status)
{
  elf_loader_host_exit(status);
}

static void k_abort(void)
{
  elf_loader_host_exit(134);
}

extern char **environ;
