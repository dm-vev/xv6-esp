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
} ksh_sig_state_t;

static SemaphoreHandle_t g_sig_state_lock;
static ksh_sig_state_t g_sig_state[KSH_SIG_SLOT_CAP];

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

static int k_sig_valid(int sig)
{
  return (sig > 0 && sig < KSH_SIG_MAX) ? 1 : 0;
}

static ksh_sig_state_t *sig_state_slot_locked(TaskHandle_t task, int create)
{
  int i;
  int free_slot = -1;
  if(task == 0)
    return 0;
  for(i = 0; i < KSH_SIG_SLOT_CAP; i++){
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
  for(i = 0; i < KSH_SIG_SLOT_CAP; i++){
    if(g_sig_state[i].task == task){
      memset(&g_sig_state[i], 0, sizeof(g_sig_state[i]));
      break;
    }
  }
  sig_state_lock_give();
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
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  ksh_sig_state_t *slot;
  void (*handler)(int) = SIG_DFL;

  if(!k_sig_valid(sig)){
    errno = EINVAL;
    return -1;
  }

  sig_state_lock_take();
  slot = sig_state_slot_locked(self, 0);
  if(slot && slot->handlers[sig])
    handler = slot->handlers[sig];
  sig_state_lock_give();

  if(handler == SIG_IGN)
    return 0;
  if(handler == SIG_DFL){
    k_exit(128 + sig);
    return 0;
  }
  handler(sig);
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
  (void)pid;
  (void)sig;
  k_reent_set_errno(r, ENOSYS);
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
