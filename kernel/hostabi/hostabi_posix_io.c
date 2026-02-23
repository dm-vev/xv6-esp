/**
 * @file hostabi_posix_io.c
 * @brief Implementation of POSIX I/O and terminal control operations
 *
 * This file implements the POSIX I/O operations that go beyond basic file
 * operations, including:
 * - File descriptor metadata tracking (flags, TTY state)
 * - Terminal (TTY) control operations
 * - fcntl operations
 * - ioctl operations
 *
 * Each file descriptor can have metadata including open flags, TTY status,
 * terminal settings (termios), and window size (winsize).
 */
#include "hostabi/hostabi_posix_io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>

#include "loader/elf_loader.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "vfs/vfs.h"

#define HOSTABI_FD_META_MAX XV6_FD_CAP
#define XV6_KSTAT_T_DEVICE 3

/**
 * @brief Window size structure for terminal
 */
typedef struct {
  unsigned short ws_row;
  unsigned short ws_col;
  unsigned short ws_xpixel;
  unsigned short ws_ypixel;
} hostabi_winsize_t;

/**
 * @brief Metadata stored for each file descriptor
 *
 * Tracks open flags, TTY status, termios settings, and window size
 * for each file descriptor in the system.
 */
typedef struct {
  int used;        /**< Whether this fd has metadata */
  int fl;          /**< Open flags (O_RDONLY, O_WRONLY, etc.) */
  int fd_flags;    /**< FD flags (FD_CLOEXEC, etc.) */
  int is_tty;      /**< Whether this is a TTY */
  hostabi_winsize_t ws; /**< Terminal window size */
  struct termios tio;   /**< Terminal attributes */
} hostabi_fd_meta_t;

static SemaphoreHandle_t g_fd_meta_lock; /**< Lock for metadata array */
static hostabi_fd_meta_t g_fd_meta[HOSTABI_FD_META_MAX]; /**< Metadata per fd */

/**
 * @brief Acquire the metadata lock
 *
 * Creates mutex on first call (lazy init), then acquires it.
 */
static void fd_meta_lock_take(void)
{
  if(g_fd_meta_lock == 0)
    g_fd_meta_lock = xSemaphoreCreateMutex();
  if(g_fd_meta_lock)
    (void)xSemaphoreTake(g_fd_meta_lock, portMAX_DELAY);
}

/**
 * @brief Release the metadata lock
 */
static void fd_meta_lock_give(void)
{
  if(g_fd_meta_lock)
    (void)xSemaphoreGive(g_fd_meta_lock);
}

/**
 * @brief Map file descriptor (passthrough)
 */
static int map_fd(int fd)
{
  return fd;
}

/**
 * @brief Check if fd is in valid range
 * @return 1 if valid, 0 otherwise
 */
static int fd_meta_in_range(int fd)
{
  return (fd >= 0 && fd < HOSTABI_FD_META_MAX);
}

/**
 * @brief Initialize termios to default settings
 * @param t Termios structure to initialize
 *
 * Sets default terminal attributes:
 * - Input flags: BRKINT, ICRNL, IXON
 * - Output flags: OPOST, ONLCR
 * - Control flags: CREAD, CS8
 * - Local flags: ECHO, ICANON, IEXTEN, ISIG
 * - Control characters: VMIN=1, VTIME=0
 */
static void fd_meta_make_termios(struct termios *t)
{
  if(t == 0)
    return;
  memset(t, 0, sizeof(*t));
  t->c_iflag = BRKINT | ICRNL | IXON;
  t->c_oflag = OPOST | ONLCR;
  t->c_cflag = CREAD | CS8;
  t->c_lflag = ECHO | ICANON | IEXTEN | ISIG;
#ifdef VMIN
  t->c_cc[VMIN] = 1;
#endif
#ifdef VTIME
  t->c_cc[VTIME] = 0;
#endif
}

/**
 * @brief Set default metadata for a new fd
 * @param fd File descriptor number
 * @param m Metadata structure to fill
 *
 * Sets defaults: marks as used, clears fd flags, determines TTY status
 * from fd number (0,1,2 are TTY), sets default open mode based on fd,
 * and initializes terminal settings.
 */
static void fd_meta_defaults_for_fd(int fd, hostabi_fd_meta_t *m)
{
  if(m == 0)
    return;
  memset(m, 0, sizeof(*m));
  m->used = 1;
  m->fd_flags = 0;
  m->is_tty = (fd >= 0 && fd <= 2) ? 1 : 0;
  if(fd == 0)
    m->fl = O_RDONLY;
  else if(fd == 1 || fd == 2)
    m->fl = O_WRONLY;
  else
    m->fl = O_RDWR;
  m->ws.ws_row = 24;
  m->ws.ws_col = 80;
  m->ws.ws_xpixel = 0;
  m->ws.ws_ypixel = 0;
  fd_meta_make_termios(&m->tio);
}

/**
 * @brief Clear metadata for an fd
 *
 * Clears all metadata, marking the slot as unused.
 */
static void fd_meta_clear_locked(int fd)
{
  if(!fd_meta_in_range(fd))
    return;
  memset(&g_fd_meta[fd], 0, sizeof(g_fd_meta[fd]));
}

/**
 * @brief Store metadata for an fd
 *
 * Copies the provided metadata into the slot for this fd.
 */
static void fd_meta_set_locked(int fd, const hostabi_fd_meta_t *m)
{
  if(!fd_meta_in_range(fd) || m == 0)
    return;
  g_fd_meta[fd] = *m;
}

/**
 * @brief Retrieve metadata for an fd
 * @param out Output structure
 * @return 0 on success, -1 if not found
 *
 * Gets metadata if it exists and the slot is in use.
 */
static int fd_meta_get_locked(int fd, hostabi_fd_meta_t *out)
{
  if(out == 0 || !fd_meta_in_range(fd) || !g_fd_meta[fd].used)
    return -1;
  *out = g_fd_meta[fd];
  return 0;
}

/**
 * @brief Check if path refers to a TTY-like device
 * @param path File path to check
 * @return 1 if TTY-like, 0 otherwise
 *
 * Recognizes /dev/tty, /dev/stdin, /dev/stdout, /dev/stderr,
 * /dev/console, /dev/ptmx, and /dev/pts/<n> as TTY devices.
 */
static int path_is_tty_like(const char *path)
{
  if(path == 0)
    return 0;
  return (strcmp(path, "/dev/tty") == 0 || strcmp(path, "/dev/stdin") == 0 || strcmp(path, "/dev/stdout") == 0 ||
          strcmp(path, "/dev/stderr") == 0 || strcmp(path, "/dev/console") == 0 || strcmp(path, "/dev/ptmx") == 0 ||
          strncmp(path, "/dev/pts/", 9) == 0);
}

/**
 * @brief Fetch metadata, creating defaults if needed
 * @param fd File descriptor
 * @param out Output structure
 * @return 0 on success, -1 if unable to fetch/create
 *
 * Tries to get existing metadata. If none exists, attempts to create
 * defaults by querying xv6 for file type (device = TTY).
 */
static int fd_meta_fetch(int fd, hostabi_fd_meta_t *out)
{
  int rc;
  int real_fd = map_fd(fd);
  fd_meta_lock_take();
  rc = fd_meta_get_locked(real_fd, out);
  if(rc != 0 && fd_meta_in_range(real_fd)){
    hostabi_fd_meta_t m;
    xv6_kstat_t st;
    if(xv6_fstat(real_fd, &st) == 0){
      fd_meta_defaults_for_fd(real_fd, &m);
      if(st.type == XV6_KSTAT_T_DEVICE)
        m.is_tty = 1;
      fd_meta_set_locked(real_fd, &m);
      if(out)
        *out = m;
      rc = 0;
    }
  }
  fd_meta_lock_give();
  return rc;
}

/**
 * @brief Store metadata for an fd
 * @return 0 on success, -1 on failure
 */
static int fd_meta_store(int fd, const hostabi_fd_meta_t *m)
{
  int real_fd = map_fd(fd);
  if(m == 0 || !fd_meta_in_range(real_fd))
    return -1;
  fd_meta_lock_take();
  fd_meta_set_locked(real_fd, m);
  fd_meta_lock_give();
  return 0;
}

/**
 * @brief Initialize the I/O subsystem
 *
 * Clears all metadata slots and sets up default metadata
 * for stdin, stdout, and stderr (fds 0, 1, 2).
 */
void hostabi_posix_io_init(void)
{
  int fd;
  fd_meta_lock_take();
  for(fd = 0; fd < HOSTABI_FD_META_MAX; fd++)
    fd_meta_clear_locked(fd);
  for(fd = 0; fd <= 2; fd++){
    hostabi_fd_meta_t m;
    fd_meta_defaults_for_fd(fd, &m);
    fd_meta_set_locked(fd, &m);
  }
  fd_meta_lock_give();
}

/**
 * @brief Called when a file is opened
 * @param fd File descriptor
 * @param flags Open flags
 * @param path File path (for TTY detection)
 *
 * Sets up metadata for the new fd, determining TTY status from path
 * and extracting access mode from flags.
 */
void hostabi_posix_io_on_open(int fd, int flags, const char *path)
{
  hostabi_fd_meta_t meta;
  fd_meta_defaults_for_fd(fd, &meta);
  meta.fl = (flags & ~(O_CREAT | O_TRUNC)) | (meta.fl & O_ACCMODE);
  if((flags & O_ACCMODE) == O_WRONLY || (flags & O_ACCMODE) == O_RDWR || (flags & O_ACCMODE) == O_RDONLY)
    meta.fl = (meta.fl & ~O_ACCMODE) | (flags & O_ACCMODE);
  meta.is_tty = path_is_tty_like(path);
  (void)fd_meta_store(fd, &meta);
}

/**
 * @brief Called when a file is closed
 * @param fd File descriptor
 * @param keep_stdio_defaults Preserve stdio defaults
 *
 * Clears metadata for the closed fd. If keep_stdio_defaults is true
 * and fd is 0, 1, or 2, restores default metadata for stdio streams.
 */
void hostabi_posix_io_on_close(int fd, int keep_stdio_defaults)
{
  fd = map_fd(fd);
  fd_meta_lock_take();
  fd_meta_clear_locked(fd);
  if(keep_stdio_defaults && fd >= 0 && fd <= 2){
    hostabi_fd_meta_t m;
    fd_meta_defaults_for_fd(fd, &m);
    fd_meta_set_locked(fd, &m);
  }
  fd_meta_lock_give();
}

/**
 * @brief Called when a file is duplicated
 * @param oldfd Original fd
 * @param newfd New fd
 *
 * Copies metadata from oldfd to newfd, or uses defaults if oldfd
 * has no metadata.
 */
void hostabi_posix_io_on_dup(int oldfd, int newfd)
{
  hostabi_fd_meta_t meta;
  if(fd_meta_fetch(oldfd, &meta) != 0)
    fd_meta_defaults_for_fd(newfd, &meta);
  (void)fd_meta_store(newfd, &meta);
}

/**
 * @brief Check if fd is a terminal
 * @param fd File descriptor
 * @return 1 if TTY, 0 otherwise
 *
 * First tries to get metadata with TTY flag. If that fails but fd
 * is 0, 1, or 2, returns true (stdio is TTY).
 */
int hostabi_posix_isatty(int fd)
{
  hostabi_fd_meta_t m;
  if(fd_meta_fetch(fd, &m) == 0){
    if(!m.is_tty)
      errno = ENOTTY;
    return m.is_tty;
  }
  if(fd >= 0 && fd <= 2)
    return 1;
  errno = EBADF;
  return 0;
}

/**
 * @brief Manipulate file descriptor flags
 * @param fd File descriptor
 * @param cmd Fcntl command
 * @param ... Optional argument
 * @return Depends on command, -1 on error
 *
 * Supports:
 * - F_GETFL: Get file status flags
 * - F_SETFL: Set file status flags (only O_APPEND, O_NONBLOCK)
 * - F_GETFD: Get file descriptor flags
 * - F_SETFD: Set file descriptor flags (only FD_CLOEXEC)
 * - F_DUPFD: Duplicate with minimum fd
 * - F_DUPFD_CLOEXEC: Duplicate with CLOEXEC and minimum fd
 */
int hostabi_posix_fcntl(int fd, int cmd, ...)
{
  va_list ap;
  long arg = 0;
  int needs_arg = 0;
  hostabi_fd_meta_t meta;

  fd = map_fd(fd);
  if(fd_meta_fetch(fd, &meta) != 0){
    errno = EBADF;
    return -1;
  }

  /* Determine if command needs an argument */
  switch(cmd){
  case F_SETFL:
  case F_SETFD:
  case F_DUPFD:
#ifdef F_DUPFD_CLOEXEC
  case F_DUPFD_CLOEXEC:
#endif
    needs_arg = 1;
    break;
  default:
    break;
  }

  if(needs_arg){
    va_start(ap, cmd);
    arg = va_arg(ap, long);
    va_end(ap);
  }

  switch(cmd){
  case F_GETFL:
    return meta.fl;
  case F_SETFL:
    {
      int status = (int)arg;
      int xv6_status = 0;
      if(status & O_APPEND)
        xv6_status |= XV6_O_APPEND;
      if(xv6_set_status_flags(fd, xv6_status) != 0){
        errno = xv6_last_errno();
        if(errno <= 0)
          errno = EBADF;
        return -1;
      }
      meta.fl = (meta.fl & ~(O_APPEND | O_NONBLOCK)) | (status & (O_APPEND | O_NONBLOCK));
    }
    (void)fd_meta_store(fd, &meta);
    return 0;
  case F_GETFD:
    return meta.fd_flags;
  case F_SETFD:
    meta.fd_flags = ((int)arg & FD_CLOEXEC);
    (void)fd_meta_store(fd, &meta);
    return 0;
  case F_DUPFD:
#ifdef F_DUPFD_CLOEXEC
  case F_DUPFD_CLOEXEC:
#endif
    if((int)arg < 0){
      errno = EINVAL;
      return -1;
    }
    {
      int i;
      int nfd = (int)arg;
      int rc = -1;
      int dups[HOSTABI_FD_META_MAX];
      int ndups = 0;
      while(1){
        rc = xv6_dup(fd);
        if(rc < 0){
          errno = xv6_last_errno();
          if(errno <= 0)
            errno = EBADF;
          goto fail_dupfd;
        }
        hostabi_posix_io_on_dup(fd, rc);
        if(rc >= nfd)
          break;
        if(ndups >= HOSTABI_FD_META_MAX){
          errno = EMFILE;
          goto fail_dupfd;
        }
        dups[ndups++] = rc;
      }
      for(i = 0; i < ndups; i++){
        (void)xv6_close(dups[i]);
        hostabi_posix_io_on_close(dups[i], 0);
      }
#ifdef F_DUPFD_CLOEXEC
      if(cmd == F_DUPFD_CLOEXEC && fd_meta_fetch(rc, &meta) == 0){
        meta.fd_flags |= FD_CLOEXEC;
        (void)fd_meta_store(rc, &meta);
      }
#endif
      return rc;

fail_dupfd:
      for(i = 0; i < ndups; i++){
        (void)xv6_close(dups[i]);
        hostabi_posix_io_on_close(dups[i], 0);
      }
      return -1;
    }
  default:
    errno = EINVAL;
    return -1;
  }
}

/**
 * @brief Get terminal attributes
 * @param fd File descriptor
 * @param tio Buffer for termios
 * @return 0 on success, -1 on failure
 *
 * Retrieves current terminal settings from metadata.
 */
int hostabi_posix_tcgetattr(int fd, struct termios *tio)
{
  hostabi_fd_meta_t meta;
  tio = (struct termios *)elf_loader_translate_ptr(tio);
  if(tio == 0){
    errno = EINVAL;
    return -1;
  }
  fd = map_fd(fd);
  if(fd_meta_fetch(fd, &meta) != 0){
    errno = EBADF;
    return -1;
  }
  if(!meta.is_tty){
    errno = ENOTTY;
    return -1;
  }
  *tio = meta.tio;
  return 0;
}

/**
 * @brief Set terminal attributes
 * @param fd File descriptor
 * @param optional_actions When to apply (TCSANOW, TCSADRAIN, TCSAFLUSH)
 * @param tio New settings
 * @return 0 on success, -1 on failure
 *
 * Sets terminal parameters. The optional_actions is validated but
 * not actually implemented differently (all apply immediately in this
 * simplified implementation).
 */
int hostabi_posix_tcsetattr(int fd, int optional_actions, const struct termios *tio)
{
  hostabi_fd_meta_t meta;
  tio = (const struct termios *)elf_loader_translate_ptr(tio);
  if(tio == 0){
    errno = EINVAL;
    return -1;
  }
  if(optional_actions != TCSANOW && optional_actions != TCSADRAIN && optional_actions != TCSAFLUSH){
    errno = EINVAL;
    return -1;
  }
  fd = map_fd(fd);
  if(fd_meta_fetch(fd, &meta) != 0){
    errno = EBADF;
    return -1;
  }
  if(!meta.is_tty){
    errno = ENOTTY;
    return -1;
  }
  meta.tio = *tio;
  (void)fd_meta_store(fd, &meta);
  return 0;
}

/**
 * @brief Configure terminal to raw mode
 * @param tio Terminal attributes
 *
 * Disables echo, canonical mode, signals, and input processing
 * to configure the terminal for raw, unbuffered operation.
 */
void hostabi_posix_cfmakeraw(struct termios *tio)
{
  tio = (struct termios *)elf_loader_translate_ptr(tio);
  if(tio == 0)
    return;
  tio->c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  tio->c_oflag &= ~(OPOST);
  tio->c_cflag |= CS8;
  tio->c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
#ifdef VMIN
  tio->c_cc[VMIN] = 1;
#endif
#ifdef VTIME
  tio->c_cc[VTIME] = 0;
#endif
}

/**
 * @brief Device control
 * @param fd File descriptor
 * @param request Ioctl request
 * @param ... Optional argument
 * @return 0 on success, -1 on failure
 *
 * Supports:
 * - FIONBIO: Set/clear non-blocking mode
 * - TIOCGWINSZ: Get window size
 * - TIOCSWINSZ: Set window size
 * - TCGETS: Get terminal settings
 * - TCSETS: Set terminal settings (now)
 * - TCSETSW: Set terminal settings (drain)
 * - TCSETSF: Set terminal settings (flush)
 */
int hostabi_posix_ioctl(int fd, unsigned long request, ...)
{
  hostabi_fd_meta_t meta;
  fd = map_fd(fd);

  if(fd_meta_fetch(fd, &meta) != 0){
    errno = EBADF;
    return -1;
  }

#ifdef FIONBIO
  if(request == (unsigned long)FIONBIO){
    va_list ap;
    void *argp;
    va_start(ap, request);
    argp = va_arg(ap, void *);
    va_end(ap);
    argp = (void *)elf_loader_translate_ptr(argp);
    int *on = (int *)argp;
    if(on == 0){
      errno = EINVAL;
      return -1;
    }
    if(*on)
      meta.fl |= O_NONBLOCK;
    else
      meta.fl &= ~O_NONBLOCK;
    (void)fd_meta_store(fd, &meta);
    return 0;
  }
#endif

#ifdef TIOCGWINSZ
  if(request == (unsigned long)TIOCGWINSZ){
    va_list ap;
    void *argp;
    va_start(ap, request);
    argp = va_arg(ap, void *);
    va_end(ap);
    argp = (void *)elf_loader_translate_ptr(argp);
    hostabi_winsize_t *ws = (hostabi_winsize_t *)argp;
    if(ws == 0){
      errno = EINVAL;
      return -1;
    }
    if(!meta.is_tty){
      errno = ENOTTY;
      return -1;
    }
    *ws = meta.ws;
    return 0;
  }
#endif
#ifdef TIOCSWINSZ
  if(request == (unsigned long)TIOCSWINSZ){
    va_list ap;
    void *argp;
    va_start(ap, request);
    argp = va_arg(ap, void *);
    va_end(ap);
    argp = (void *)elf_loader_translate_ptr(argp);
    const hostabi_winsize_t *ws = (const hostabi_winsize_t *)argp;
    if(ws == 0){
      errno = EINVAL;
      return -1;
    }
    if(!meta.is_tty){
      errno = ENOTTY;
      return -1;
    }
    meta.ws = *ws;
    (void)fd_meta_store(fd, &meta);
    return 0;
  }
#endif

#ifdef TCGETS
  if(request == (unsigned long)TCGETS){
    va_list ap;
    void *argp;
    va_start(ap, request);
    argp = va_arg(ap, void *);
    va_end(ap);
    argp = (void *)elf_loader_translate_ptr(argp);
    return hostabi_posix_tcgetattr(fd, (struct termios *)argp);
  }
#endif
#ifdef TCSETS
  if(request == (unsigned long)TCSETS){
    va_list ap;
    void *argp;
    va_start(ap, request);
    argp = va_arg(ap, void *);
    va_end(ap);
    argp = (void *)elf_loader_translate_ptr(argp);
    return hostabi_posix_tcsetattr(fd, TCSANOW, (const struct termios *)argp);
  }
#endif
#ifdef TCSETSW
  if(request == (unsigned long)TCSETSW){
    va_list ap;
    void *argp;
    va_start(ap, request);
    argp = va_arg(ap, void *);
    va_end(ap);
    argp = (void *)elf_loader_translate_ptr(argp);
    return hostabi_posix_tcsetattr(fd, TCSADRAIN, (const struct termios *)argp);
  }
#endif
#ifdef TCSETSF
  if(request == (unsigned long)TCSETSF){
    va_list ap;
    void *argp;
    va_start(ap, request);
    argp = va_arg(ap, void *);
    va_end(ap);
    argp = (void *)elf_loader_translate_ptr(argp);
    return hostabi_posix_tcsetattr(fd, TCSAFLUSH, (const struct termios *)argp);
  }
#endif

  errno = ENOTTY;
  return -1;
}
