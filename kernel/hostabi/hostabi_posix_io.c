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
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>

#include "core/param.h"
#include "loader/elf_loader.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "platform/hal.h"
#include "vfs/vfs.h"

#define HOSTABI_FD_META_MAX XV6_FD_CAP

#ifndef TIOCGWINSZ
#define TIOCGWINSZ 0x5413UL
#endif
#ifndef TIOCSWINSZ
#define TIOCSWINSZ 0x5414UL
#endif

enum {
  HOSTABI_TTY_KIND_NONE = 0,
  HOSTABI_TTY_KIND_CONSOLE,
  HOSTABI_TTY_KIND_PTY,
};

/**
 * @brief Window size structure for terminal
 */
typedef struct {
  unsigned short ws_row;
  unsigned short ws_col;
  unsigned short ws_xpixel;
  unsigned short ws_ypixel;
} hostabi_winsize_t;

typedef struct {
  int kind;
  int id;
} hostabi_tty_ref_t;

typedef struct {
  int valid;
  hostabi_winsize_t ws;
  struct termios tio;
} hostabi_tty_state_t;

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
  hostabi_tty_ref_t tty_ref; /**< Shared TTY state key */
} hostabi_fd_meta_t;

static SemaphoreHandle_t g_fd_meta_lock; /**< Lock for metadata array */
static hostabi_fd_meta_t *g_fd_meta;     /**< Metadata per fd */
static int g_fd_meta_cap;                /**< Allocated metadata slots */
static hostabi_posix_tty_signal_handler_t g_tty_signal_handler;
static hostabi_posix_tty_foreground_query_t g_tty_foreground_query;
static hostabi_tty_state_t g_console_tty;
static hostabi_tty_state_t g_pty_tty[XV6_PTY_CAP];

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

static void *fd_meta_alloc_data(size_t sz)
{
  void *p = 0;
#ifdef MALLOC_CAP_SPIRAM
  p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  if(p == 0)
    p = heap_caps_malloc(sz, MALLOC_CAP_8BIT);
  return p;
}

static int fd_meta_ensure_capacity_locked(int min_cap)
{
  hostabi_fd_meta_t *new_meta;
  int new_cap;

  if(min_cap <= 0 || min_cap > HOSTABI_FD_META_MAX)
    return -1;
  if(min_cap <= g_fd_meta_cap)
    return 0;

  new_cap = (g_fd_meta_cap > 0) ? g_fd_meta_cap : 8;
  while(new_cap < min_cap){
    int next = new_cap * 2;
    if(next <= 0 || next > HOSTABI_FD_META_MAX)
      next = HOSTABI_FD_META_MAX;
    if(next == new_cap)
      break;
    new_cap = next;
  }
  if(new_cap < min_cap)
    return -1;

  new_meta = (hostabi_fd_meta_t *)fd_meta_alloc_data((size_t)new_cap * sizeof(*new_meta));
  if(new_meta == 0)
    return -1;
  memset(new_meta, 0, (size_t)new_cap * sizeof(*new_meta));
  if(g_fd_meta && g_fd_meta_cap > 0)
    memcpy(new_meta, g_fd_meta, (size_t)g_fd_meta_cap * sizeof(*new_meta));
  if(g_fd_meta)
    heap_caps_free(g_fd_meta);
  g_fd_meta = new_meta;
  g_fd_meta_cap = new_cap;
  return 0;
}

static void fd_meta_reset_table_locked(void)
{
  if(g_fd_meta){
    heap_caps_free(g_fd_meta);
    g_fd_meta = 0;
  }
  g_fd_meta_cap = 0;
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
static void tty_make_termios_defaults(struct termios *t)
{
  if(t == 0)
    return;
  memset(t, 0, sizeof(*t));
  t->c_iflag = BRKINT | ICRNL | IXON;
  t->c_oflag = OPOST | ONLCR;
  t->c_cflag = CREAD | CS8;
  t->c_lflag = ECHO | ICANON | IEXTEN | ISIG;
#ifdef VINTR
  t->c_cc[VINTR] = 0x03;
#endif
#ifdef VSUSP
  t->c_cc[VSUSP] = 0x1a;
#endif
#ifdef VMIN
  t->c_cc[VMIN] = 1;
#endif
#ifdef VTIME
  t->c_cc[VTIME] = 0;
#endif
}

static void tty_state_make_defaults(hostabi_tty_state_t *state)
{
  if(state == 0)
    return;
  memset(state, 0, sizeof(*state));
  state->valid = 1;
  state->ws.ws_row = 24;
  state->ws.ws_col = 80;
  state->ws.ws_xpixel = 0;
  state->ws.ws_ypixel = 0;
  tty_make_termios_defaults(&state->tio);
}

static void tty_state_reset_all_locked(void)
{
  int i;

  tty_state_make_defaults(&g_console_tty);
  for(i = 0; i < XV6_PTY_CAP; i++)
    memset(&g_pty_tty[i], 0, sizeof(g_pty_tty[i]));
}

static void tty_ref_clear(hostabi_tty_ref_t *ref)
{
  if(ref == 0)
    return;
  ref->kind = HOSTABI_TTY_KIND_NONE;
  ref->id = -1;
}

static void tty_ref_set_console(hostabi_tty_ref_t *ref)
{
  if(ref == 0)
    return;
  ref->kind = HOSTABI_TTY_KIND_CONSOLE;
  ref->id = 0;
}

static void tty_ref_set_pty(hostabi_tty_ref_t *ref, int id)
{
  if(ref == 0)
    return;
  ref->kind = HOSTABI_TTY_KIND_PTY;
  ref->id = id;
}

static int tty_ref_is_valid(const hostabi_tty_ref_t *ref)
{
  if(ref == 0)
    return 0;
  if(ref->kind == HOSTABI_TTY_KIND_CONSOLE)
    return 1;
  if(ref->kind == HOSTABI_TTY_KIND_PTY)
    return (ref->id >= 0 && ref->id < XV6_PTY_CAP) ? 1 : 0;
  return 0;
}

static hostabi_tty_state_t *tty_state_for_ref_locked(const hostabi_tty_ref_t *ref)
{
  if(!tty_ref_is_valid(ref))
    return 0;
  if(ref->kind == HOSTABI_TTY_KIND_CONSOLE)
    return &g_console_tty;
  if(ref->kind == HOSTABI_TTY_KIND_PTY)
    return &g_pty_tty[ref->id];
  return 0;
}

static int tty_state_get_locked(const hostabi_tty_ref_t *ref, hostabi_tty_state_t *out)
{
  hostabi_tty_state_t *state = tty_state_for_ref_locked(ref);

  if(state == 0 || out == 0)
    return -1;
  if(!state->valid)
    tty_state_make_defaults(state);
  *out = *state;
  return 0;
}

static int tty_state_set_locked(const hostabi_tty_ref_t *ref, const hostabi_tty_state_t *state)
{
  hostabi_tty_state_t *dst = tty_state_for_ref_locked(ref);

  if(dst == 0 || state == 0)
    return -1;
  *dst = *state;
  dst->valid = 1;
  return 0;
}

static int path_is_console_tty(const char *path)
{
  if(path == 0)
    return 0;
  return (strcmp(path, "/dev/tty") == 0 || strcmp(path, "/dev/stdin") == 0 || strcmp(path, "/dev/stdout") == 0 ||
          strcmp(path, "/dev/stderr") == 0 || strcmp(path, "/dev/console") == 0);
}

static int parse_pts_id_from_path(const char *path, int *out_id)
{
  int id;
  const char *p;

  if(path == 0 || out_id == 0)
    return -1;
  if(strncmp(path, "/dev/pts/", 9) != 0)
    return -1;
  p = path + 9;
  if(*p < '0' || *p > '9')
    return -1;

  id = 0;
  while(*p >= '0' && *p <= '9'){
    id = (id * 10) + (*p - '0');
    if(id >= XV6_PTY_CAP)
      return -1;
    p++;
  }
  if(*p != 0)
    return -1;
  *out_id = id;
  return 0;
}

static int tty_ref_from_path_fd_locked(int fd, const char *path, hostabi_tty_ref_t *out_ref)
{
  char pts_path[MAXPATH];
  int pty_id = -1;

  if(out_ref == 0)
    return -1;
  tty_ref_clear(out_ref);

  if(path_is_console_tty(path)){
    tty_ref_set_console(out_ref);
    return 0;
  }
  if(path && parse_pts_id_from_path(path, &pty_id) == 0){
    tty_ref_set_pty(out_ref, pty_id);
    return 0;
  }
  if(path && strcmp(path, "/dev/ptmx") == 0){
    if(xv6_ptsname(fd, pts_path, sizeof(pts_path)) == 0 && parse_pts_id_from_path(pts_path, &pty_id) == 0){
      tty_ref_set_pty(out_ref, pty_id);
      return 0;
    }
  }

  return -1;
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
  tty_ref_clear(&m->tty_ref);
  if(fd >= 0 && fd <= 2)
    tty_ref_set_console(&m->tty_ref);
}

/**
 * @brief Clear metadata for an fd
 *
 * Clears all metadata, marking the slot as unused.
 */
static void fd_meta_clear_locked(int fd)
{
  if(!fd_meta_in_range(fd) || fd >= g_fd_meta_cap || g_fd_meta == 0)
    return;
  memset(&g_fd_meta[fd], 0, sizeof(g_fd_meta[0]));
}

/**
 * @brief Store metadata for an fd
 *
 * Copies the provided metadata into the slot for this fd.
 */
static int fd_meta_set_locked(int fd, const hostabi_fd_meta_t *m)
{
  if(!fd_meta_in_range(fd) || m == 0)
    return -1;
  if(fd_meta_ensure_capacity_locked(fd + 1) != 0 || g_fd_meta == 0)
    return -1;
  g_fd_meta[fd] = *m;
  return 0;
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
  if(out == 0 || !fd_meta_in_range(fd) || fd >= g_fd_meta_cap || g_fd_meta == 0 || !g_fd_meta[fd].used)
    return -1;
  *out = g_fd_meta[fd];
  return 0;
}

static int tty_ref_from_fd_locked(int fd, hostabi_tty_ref_t *out_ref)
{
  char path[MAXPATH];

  if(out_ref == 0)
    return -1;
  if(fd >= 0 && fd <= 2){
    tty_ref_set_console(out_ref);
    return 0;
  }
  if(xv6_fd_path(fd, path, sizeof(path)) != 0){
    tty_ref_clear(out_ref);
    return -1;
  }
  return tty_ref_from_path_fd_locked(fd, path, out_ref);
}

/**
 * @brief Fetch metadata, creating defaults if needed
 * @param fd File descriptor
 * @param out Output structure
 * @return 0 on success, -1 if unable to fetch/create
 *
 * Tries to get existing metadata. If none exists, attempts to create
 * defaults by resolving the current fd path into a shared TTY key.
 */
static int fd_meta_fetch(int fd, hostabi_fd_meta_t *out)
{
  int rc;
  int real_fd = map_fd(fd);
  fd_meta_lock_take();
  rc = fd_meta_get_locked(real_fd, out);
  if(rc != 0 && fd_meta_in_range(real_fd)){
    hostabi_fd_meta_t m;
    char path[MAXPATH];
    int have_path = 0;

    if(real_fd >= 0 && real_fd <= 2)
      have_path = 1;
    else if(xv6_fd_path(real_fd, path, sizeof(path)) == 0)
      have_path = 1;

    if(have_path){
      fd_meta_defaults_for_fd(real_fd, &m);
      if(tty_ref_from_fd_locked(real_fd, &m.tty_ref) == 0)
        m.is_tty = 1;
      else
        tty_ref_clear(&m.tty_ref);
      if(fd_meta_set_locked(real_fd, &m) == 0){
        if(out)
          *out = m;
        rc = 0;
      }
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
  int rc;
  int real_fd = map_fd(fd);
  if(m == 0 || !fd_meta_in_range(real_fd))
    return -1;
  fd_meta_lock_take();
  rc = fd_meta_set_locked(real_fd, m);
  fd_meta_lock_give();
  return rc;
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
  fd_meta_reset_table_locked();
  tty_state_reset_all_locked();
  if(fd_meta_ensure_capacity_locked(3) != 0){
    fd_meta_lock_give();
    return;
  }
  for(fd = 0; fd <= 2; fd++){
    hostabi_fd_meta_t m;
    fd_meta_defaults_for_fd(fd, &m);
    (void)fd_meta_set_locked(fd, &m);
  }
  fd_meta_lock_give();
  g_tty_signal_handler = 0;
  g_tty_foreground_query = 0;
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
  if(tty_ref_from_path_fd_locked(fd, path, &meta.tty_ref) == 0)
    meta.is_tty = 1;
  else
    tty_ref_clear(&meta.tty_ref);
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
    (void)fd_meta_set_locked(fd, &m);
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

static int tty_state_snapshot_for_fd(int fd, hostabi_fd_meta_t *out_meta, hostabi_tty_state_t *out_state)
{
  hostabi_fd_meta_t meta;
  hostabi_tty_state_t state;

  if(fd_meta_fetch(fd, &meta) != 0){
    errno = EBADF;
    return -1;
  }
  if(!meta.is_tty || !tty_ref_is_valid(&meta.tty_ref)){
    errno = ENOTTY;
    return -1;
  }

  fd_meta_lock_take();
  if(tty_state_get_locked(&meta.tty_ref, &state) != 0){
    fd_meta_lock_give();
    errno = ENOTTY;
    return -1;
  }
  fd_meta_lock_give();

  if(out_meta)
    *out_meta = meta;
  if(out_state)
    *out_state = state;
  return 0;
}

static int tty_state_store_for_fd(int fd, const hostabi_tty_state_t *state)
{
  hostabi_fd_meta_t meta;
  int rc;

  if(state == 0){
    errno = EINVAL;
    return -1;
  }
  if(fd_meta_fetch(fd, &meta) != 0){
    errno = EBADF;
    return -1;
  }
  if(!meta.is_tty || !tty_ref_is_valid(&meta.tty_ref)){
    errno = ENOTTY;
    return -1;
  }

  fd_meta_lock_take();
  rc = tty_state_set_locked(&meta.tty_ref, state);
  fd_meta_lock_give();
  if(rc != 0){
    errno = ENOTTY;
    return -1;
  }
  return 0;
}

void hostabi_posix_set_tty_foreground_query(hostabi_posix_tty_foreground_query_t query)
{
  g_tty_foreground_query = query;
}

enum {
  HOSTABI_TTY_ACCESS_READ = 0,
  HOSTABI_TTY_ACCESS_WRITE,
  HOSTABI_TTY_ACCESS_ATTR,
};

static int tty_wait_access(int fd, int access)
{
  hostabi_tty_state_t state;
  int fg;

  if(tty_state_snapshot_for_fd(fd, 0, &state) != 0)
    return (errno == ENOTTY) ? 0 : -1;
  if(g_tty_foreground_query == 0)
    return 0;

  while(1){
    fg = g_tty_foreground_query(fd);
    if(fg > 0)
      return 0;
    if(fg < 0){
      if(errno == 0)
        errno = EIO;
      return -1;
    }

    if(access == HOSTABI_TTY_ACCESS_WRITE){
#ifdef TOSTOP
      if((state.tio.c_lflag & TOSTOP) == 0)
        return 0;
#else
      return 0;
#endif
    }

    if(hostabi_posix_tty_dispatch_signal((access == HOSTABI_TTY_ACCESS_READ) ? SIGTTIN : SIGTTOU) != 0)
      return -1;

    if(tty_state_snapshot_for_fd(fd, 0, &state) != 0)
      return -1;
  }
}

int hostabi_posix_tty_before_read(int fd)
{
  fd = map_fd(fd);
  return tty_wait_access(fd, HOSTABI_TTY_ACCESS_READ);
}

int hostabi_posix_tty_before_write(int fd)
{
  fd = map_fd(fd);
  return tty_wait_access(fd, HOSTABI_TTY_ACCESS_WRITE);
}

int hostabi_posix_tty_before_attr_change(int fd)
{
  fd = map_fd(fd);
  return tty_wait_access(fd, HOSTABI_TTY_ACCESS_ATTR);
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
      if(fd_meta_fetch(rc, &meta) == 0){
#ifdef F_DUPFD_CLOEXEC
        if(cmd == F_DUPFD_CLOEXEC)
          meta.fd_flags |= FD_CLOEXEC;
        else
#endif
          meta.fd_flags &= ~FD_CLOEXEC;
        (void)fd_meta_store(rc, &meta);
      }
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
  hostabi_tty_state_t state;
  tio = (struct termios *)elf_loader_translate_ptr(tio);
  if(tio == 0){
    errno = EINVAL;
    return -1;
  }
  fd = map_fd(fd);
  if(tty_state_snapshot_for_fd(fd, 0, &state) != 0)
    return -1;
  *tio = state.tio;
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
  hostabi_tty_state_t state;
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
  if(hostabi_posix_tty_before_attr_change(fd) != 0)
    return -1;
  if(tty_state_snapshot_for_fd(fd, 0, &state) != 0)
    return -1;
  state.tio = *tio;
  if(tty_state_store_for_fd(fd, &state) != 0)
    return -1;
  if(optional_actions == TCSAFLUSH){
    if(xv6_tty_flush_input(fd) != 0){
      errno = xv6_last_errno();
      if(errno <= 0)
        errno = EIO;
      return -1;
    }
  }
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

void hostabi_posix_set_tty_signal_handler(hostabi_posix_tty_signal_handler_t handler)
{
  g_tty_signal_handler = handler;
}

static int tty_cc_enabled(cc_t cc)
{
#ifdef _POSIX_VDISABLE
  if(cc == (cc_t)_POSIX_VDISABLE)
    return 0;
#endif
  return 1;
}

int hostabi_posix_tty_signal_for_char(int c)
{
  hostabi_tty_state_t state;
  unsigned char uc;

  if(c < 0 || c > 0xff)
    return 0;
  if(tty_state_snapshot_for_fd(0, 0, &state) != 0)
    return 0;
  if((state.tio.c_lflag & ISIG) == 0)
    return 0;

  uc = (unsigned char)c;
#ifdef VINTR
  if(tty_cc_enabled(state.tio.c_cc[VINTR]) && uc == (unsigned char)state.tio.c_cc[VINTR])
    return SIGINT;
#endif
#ifdef VSUSP
  if(tty_cc_enabled(state.tio.c_cc[VSUSP]) && uc == (unsigned char)state.tio.c_cc[VSUSP])
    return SIGTSTP;
#endif
  return 0;
}

int hostabi_posix_tty_dispatch_signal(int sig)
{
  hostabi_posix_tty_signal_handler_t handler = g_tty_signal_handler;
  if(handler == 0){
    errno = ENOSYS;
    return -1;
  }
  if(handler(sig) != 0){
    if(errno == 0)
      errno = EIO;
    return -1;
  }
  return 0;
}

int hostabi_posix_tty_poll_signal(void)
{
  hostabi_tty_state_t state;
  int vintr = -1;
  int vsusp = -1;

  if(tty_state_snapshot_for_fd(0, 0, &state) != 0)
    return 0;
  if((state.tio.c_lflag & ISIG) == 0)
    return 0;

#ifdef VINTR
  if(tty_cc_enabled(state.tio.c_cc[VINTR]))
    vintr = (unsigned char)state.tio.c_cc[VINTR];
#endif
#ifdef VSUSP
  if(tty_cc_enabled(state.tio.c_cc[VSUSP]))
    vsusp = (unsigned char)state.tio.c_cc[VSUSP];
#endif

  if(vintr >= 0 && hal_console_poll_byte(vintr))
    return SIGINT;
  if(vsusp >= 0 && vsusp != vintr && hal_console_poll_byte(vsusp))
    return SIGTSTP;
  return 0;
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
  hostabi_tty_state_t state;
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
    if(tty_state_snapshot_for_fd(fd, 0, &state) != 0)
      return -1;
    *ws = state.ws;
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
    if(hostabi_posix_tty_before_attr_change(fd) != 0)
      return -1;
    if(tty_state_snapshot_for_fd(fd, 0, &state) != 0)
      return -1;
    state.ws = *ws;
    if(tty_state_store_for_fd(fd, &state) != 0)
      return -1;
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
