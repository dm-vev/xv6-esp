#include "vfs/vfs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "platform/esp_flash_disk.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_heap_caps.h"
#include "hostabi/hostabi_posix_io.h"
#include "loader/elf_loader.h"
#include "fs/fs.h"
#include "platform/hal.h"
#include "core/param.h"
#include "fs/stat.h"

/**
 * @file xv6fs_ro.c
 * @brief Read-only xv6 virtual file system implementation
 *
 * This file implements a lightweight VFS layer for xv6 on ESP32. It provides:
 * - Flash-based read-only filesystem with directory iteration
 * - File descriptor abstraction for user programs
 * - PTY (pseudo-terminal) support for shell
 * - Pipe support for inter-process communication
 * - Task context management for FreeRTOS integration
 *
 * Architecture:
 * - Superblock and inode reading from flash storage
 * - File descriptor table per-task
 * - Device abstraction for /dev/xxx special files
 * - PTY master/slave pair management
 * - Circular buffer pipes
 *
 * The VFS is designed to be minimal while providing POSIX-like API
 * for user programs running under xv6.
 */

static const char *TAG = "xv6fs";

static struct superblock g_sb;
static int g_ready;
static uint32 g_nbitmap;
static uint32 g_data_start;
static SemaphoreHandle_t g_vfs_lock;
static SemaphoreHandle_t g_ctx_lock;

#define VFS_PATH_MAX 320
#define VFS_PATH_MAX_SEGS ((VFS_PATH_MAX / 2) + 2)
#define VFS_PATH_SRC_FLAG 0x40000000
#define VFS_PATH_IDX_MASK 0x3fffffff

typedef struct {
  int stdio_active;
  int in_fd;
  int out_fd;
  int err_fd;
  int last_errno;
  char cwd[VFS_PATH_MAX];
} xv6_task_ctx_t;

#define XV6_MAX_TASK_CTX XV6_TASK_CTX_CAP

typedef struct {
  TaskHandle_t task;
  xv6_task_ctx_t ctx;
} xv6_task_ctx_slot_t;

static xv6_task_ctx_slot_t *g_task_ctx;
static int g_task_ctx_cap;

#define XV6_MAX_FD XV6_FD_CAP
#define VFD_FREE 0
#define VFD_FILE 1
#define VFD_DEV 2
#define VFD_PIPE 3
#define XV6_MAX_PTY XV6_PTY_CAP
#define XV6_MAX_PIPE XV6_PIPE_CAP

#define DEV_ROLE_NONE 0
#define DEV_ROLE_PTY_MASTER 1
#define DEV_ROLE_PTY_SLAVE 2

typedef struct {
  int used;
  int kind;
  int flags;
  int dev_role;
  int dev_id;
  int group_id;
  TaskHandle_t owner;
  uint32 inum;
  uint32 off;
  char path[MAXPATH];
} xv6_vfd_t;

static xv6_vfd_t *g_fds;
static int g_fds_cap;
static int g_next_fd_group = 1;

#define XV6_INODE_SIZE_MASK 0x000fffffu
#define XV6_INODE_MODE_MASK 0xfff00000u
#define XV6_INODE_MODE_SHIFT 20

#define XV6_DEFAULT_FILE_MODE 0666u
#define XV6_DEFAULT_DIR_MODE 0777u
#define XV6_DEFAULT_SYMLINK_MODE 0777u
#define XV6_DEFAULT_DEV_MODE 0666u
#define XV6_DEFAULT_FIFO_MODE 0666u

#define XV6_SYMLINK_MAX_DEPTH 8
#define XV6_SYMLINK_TARGET_MAX VFS_PATH_MAX

static void copy_cstr(char *dst, int dst_len, const char *src)
{
  if(dst == 0 || dst_len <= 0)
    return;
  if(src == 0)
    src = "";
  strncpy(dst, src, (size_t)dst_len - 1u);
  dst[dst_len - 1] = 0;
}

static uint16 inode_default_mode(short type)
{
  if(type == T_DIR)
    return (uint16)XV6_DEFAULT_DIR_MODE;
  if(type == T_SYMLINK)
    return (uint16)XV6_DEFAULT_SYMLINK_MODE;
  if(type == T_DEVICE)
    return (uint16)XV6_DEFAULT_DEV_MODE;
  if(type == T_FIFO)
    return (uint16)XV6_DEFAULT_FIFO_MODE;
  return (uint16)XV6_DEFAULT_FILE_MODE;
}

static uint32 inode_get_size(const struct dinode *ip)
{
  if(ip == 0)
    return 0;
  return ip->size & XV6_INODE_SIZE_MASK;
}

static void inode_set_size(struct dinode *ip, uint32 size)
{
  uint32 mode_bits;
  if(ip == 0)
    return;
  mode_bits = ip->size & XV6_INODE_MODE_MASK;
  ip->size = mode_bits | (size & XV6_INODE_SIZE_MASK);
}

static uint16 inode_get_mode(const struct dinode *ip)
{
  uint16 mode;
  if(ip == 0)
    return 0;
  mode = (uint16)((ip->size & XV6_INODE_MODE_MASK) >> XV6_INODE_MODE_SHIFT);
  if(mode == 0)
    return inode_default_mode(ip->type);
  return (uint16)(mode & 07777u);
}

static void inode_set_mode(struct dinode *ip, uint16 mode)
{
  uint32 size_bits;
  if(ip == 0)
    return;
  size_bits = ip->size & XV6_INODE_SIZE_MASK;
  ip->size = size_bits | (((uint32)(mode & 07777u)) << XV6_INODE_MODE_SHIFT);
}

static uid_t inode_get_uid(const struct dinode *ip)
{
  if(ip == 0)
    return 0;
  return (uid_t)(uint16)ip->major;
}

static gid_t inode_get_gid(const struct dinode *ip)
{
  if(ip == 0)
    return 0;
  return (gid_t)(uint16)ip->minor;
}

static void inode_set_uid_gid(struct dinode *ip, int owner, int group)
{
  if(ip == 0)
    return;
  if(owner >= 0)
    ip->major = (short)((uint16)owner);
  if(group >= 0)
    ip->minor = (short)((uint16)group);
}

static void inode_init_attrs(struct dinode *ip, short type)
{
  if(ip == 0)
    return;
  inode_set_mode(ip, inode_default_mode(type));
  ip->major = 0;
  ip->minor = 0;
}

static int ptr_byte_readable(const void *p)
{
  if(p == 0)
    return 0;
  if(esp_ptr_byte_accessible(p))
    return 1;
  if(esp_ptr_in_drom(p))
    return 1;
  return 0;
}

static int copy_guest_cstr(const char *src, char *dst, int dst_len)
{
  int i;

  if(src == 0 || dst == 0 || dst_len <= 1)
    return -1;
  src = (const char *)elf_loader_translate_ptr(src);
  if(src == 0)
    return -1;

  for(i = 0; i < dst_len - 1; i++){
    if(!ptr_byte_readable(src + i))
      return -1;
    dst[i] = src[i];
    if(dst[i] == 0)
      return 0;
  }
  dst[dst_len - 1] = 0;
  return -1;
}

typedef struct {
  int alloc;
  int master_open;
  int slave_open;
  uint8 *m2s;
  uint16 m2s_r;
  uint16 m2s_w;
  uint16 m2s_n;
  uint8 *s2m;
  uint16 s2m_r;
  uint16 s2m_w;
  uint16 s2m_n;
} xv6_pty_t;

static xv6_pty_t g_ptys[XV6_MAX_PTY];

typedef struct {
  int alloc;
  int readers;
  int writers;
  uint8 *data;
  uint16 r;
  uint16 w;
  uint16 n;
} xv6_pipe_t;

static xv6_pipe_t g_pipes[XV6_MAX_PIPE];

typedef struct {
  int used;
  uint32 inum;
  int pipe_id;
} xv6_fifo_link_t;

static xv6_fifo_link_t g_fifo_links[XV6_MAX_PIPE];

#define XV6_PTY_BUF_CAP 256u
#define XV6_PIPE_BUF_CAP 512u

static void *vfs_alloc_data(size_t sz)
{
  void *p = 0;
#ifdef MALLOC_CAP_SPIRAM
  p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  if(p == 0)
    p = heap_caps_malloc(sz, MALLOC_CAP_8BIT);
  return p;
}

static void pty_slot_reset(xv6_pty_t *p)
{
  if(p == 0)
    return;
  if(p->m2s){
    heap_caps_free(p->m2s);
    p->m2s = 0;
  }
  if(p->s2m){
    heap_caps_free(p->s2m);
    p->s2m = 0;
  }
  memset(p, 0, sizeof(*p));
}

static void pipe_slot_reset(xv6_pipe_t *p)
{
  if(p == 0)
    return;
  if(p->data){
    heap_caps_free(p->data);
    p->data = 0;
  }
  memset(p, 0, sizeof(*p));
}

static int fd_table_ensure_locked(void)
{
  if(g_fds)
    return 0;
  g_fds = (xv6_vfd_t *)vfs_alloc_data((size_t)XV6_MAX_FD * sizeof(*g_fds));
  if(g_fds == 0)
    return -1;
  memset(g_fds, 0, (size_t)XV6_MAX_FD * sizeof(*g_fds));
  g_fds_cap = XV6_MAX_FD;
  return 0;
}

static int task_ctx_ensure_table_locked(void)
{
  if(g_task_ctx)
    return 0;
  g_task_ctx = (xv6_task_ctx_slot_t *)vfs_alloc_data((size_t)XV6_MAX_TASK_CTX * sizeof(*g_task_ctx));
  if(g_task_ctx == 0)
    return -1;
  memset(g_task_ctx, 0, (size_t)XV6_MAX_TASK_CTX * sizeof(*g_task_ctx));
  g_task_ctx_cap = XV6_MAX_TASK_CTX;
  return 0;
}

static void task_ctx_lock(void)
{
  if(g_ctx_lock == 0)
    g_ctx_lock = xSemaphoreCreateMutex();
  if(g_ctx_lock)
    (void)xSemaphoreTake(g_ctx_lock, portMAX_DELAY);
}

static void task_ctx_unlock(void)
{
  if(g_ctx_lock)
    (void)xSemaphoreGive(g_ctx_lock);
}

static xv6_task_ctx_t *task_ctx_get(int create)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  int i;
  int free_slot = -1;

  task_ctx_lock();
  if(task_ctx_ensure_table_locked() != 0){
    task_ctx_unlock();
    return 0;
  }

  for(i = 0; i < g_task_ctx_cap; i++){
    if(g_task_ctx[i].task == self){
      task_ctx_unlock();
      return &g_task_ctx[i].ctx;
    }
    if(g_task_ctx[i].task == 0 && free_slot < 0)
      free_slot = i;
  }

  if(create && free_slot >= 0){
    g_task_ctx[free_slot].task = self;
    memset(&g_task_ctx[free_slot].ctx, 0, sizeof(g_task_ctx[free_slot].ctx));
    g_task_ctx[free_slot].ctx.in_fd = 0;
    g_task_ctx[free_slot].ctx.out_fd = 1;
    g_task_ctx[free_slot].ctx.err_fd = 2;
    copy_cstr(g_task_ctx[free_slot].ctx.cwd, sizeof(g_task_ctx[free_slot].ctx.cwd), "/");
    task_ctx_unlock();
    return &g_task_ctx[free_slot].ctx;
  }

  task_ctx_unlock();
  return 0;
}

static void task_ctx_reset_all(void)
{
  int i;

  task_ctx_lock();
  if(task_ctx_ensure_table_locked() != 0){
    task_ctx_unlock();
    return;
  }
  for(i = 0; i < g_task_ctx_cap; i++){
    if(g_task_ctx[i].task){
      g_task_ctx[i].ctx.stdio_active = 0;
      g_task_ctx[i].ctx.in_fd = 0;
      g_task_ctx[i].ctx.out_fd = 1;
      g_task_ctx[i].ctx.err_fd = 2;
      g_task_ctx[i].ctx.last_errno = 0;
      copy_cstr(g_task_ctx[i].ctx.cwd, sizeof(g_task_ctx[i].ctx.cwd), "/");
    }
  }
  task_ctx_unlock();
}

static void task_ctx_set_errno(int err)
{
  xv6_task_ctx_t *ctx;
  if(err <= 0)
    err = EIO;
  ctx = task_ctx_get(1);
  if(ctx)
    ctx->last_errno = err;
}

static void task_ctx_clear_errno(void)
{
  xv6_task_ctx_t *ctx = task_ctx_get(1);
  if(ctx)
    ctx->last_errno = 0;
}

int xv6_last_errno(void)
{
  xv6_task_ctx_t *ctx = task_ctx_get(0);
  if(ctx && ctx->last_errno > 0)
    return ctx->last_errno;
  return EIO;
}

static int stdio_map_fd(int fd)
{
  xv6_task_ctx_t *ctx = task_ctx_get(0);
  if(ctx == 0 || !ctx->stdio_active)
    return fd;
  if(fd == 0)
    return ctx->in_fd;
  if(fd == 1)
    return ctx->out_fd;
  if(fd == 2)
    return ctx->err_fd;
  return fd;
}

static int path_resolve(const char *path, char *out, int out_len)
{
  char path_buf[VFS_PATH_MAX];
  const char *p;
  const char *prefix = "/";
  xv6_task_ctx_t *ctx;
  int stack[VFS_PATH_MAX_SEGS];
  int nseg = 0;

  if(path == 0 || out == 0 || out_len <= 1)
    return -1;
  if(copy_guest_cstr(path, path_buf, sizeof(path_buf)) != 0)
    return -1;
  if(path_buf[0] == 0)
    return -1;
  path = path_buf;

  ctx = task_ctx_get(1);
  if(path[0] != '/'){
    if(ctx && ctx->cwd[0])
      prefix = ctx->cwd;
    else
      prefix = "/";
  }

  p = prefix;
  while(*p){
    while(*p == '/')
      p++;
    if(*p == 0)
      break;
    if(nseg >= (int)(sizeof(stack) / sizeof(stack[0])))
      return -1;
    stack[nseg++] = (int)(p - prefix);
    while(*p && *p != '/')
      p++;
  }

  p = path;
  while(*p){
    const char *seg;
    int seg_len = 0;
    while(*p == '/')
      p++;
    if(*p == 0)
      break;
    seg = p;
    while(*p && *p != '/'){
      p++;
      seg_len++;
    }
    if(seg_len == 1 && seg[0] == '.')
      continue;
    if(seg_len == 2 && seg[0] == '.' && seg[1] == '.'){
      if(nseg > 0)
        nseg--;
      continue;
    }
    if(nseg >= (int)(sizeof(stack) / sizeof(stack[0])))
      return -1;
    stack[nseg++] = (int)(seg - path) | VFS_PATH_SRC_FLAG;
  }

  {
    int i;
    int pos = 0;
    out[pos++] = '/';
    for(i = 0; i < nseg; i++){
      const char *src;
      int len = 0;
      int idx = stack[i];
      if((idx & VFS_PATH_SRC_FLAG) != 0){
        src = path + (idx & VFS_PATH_IDX_MASK);
        while(src[len] && src[len] != '/')
          len++;
      } else {
        src = prefix + idx;
        while(src[len] && src[len] != '/')
          len++;
      }
      if(len <= 0)
        continue;
      if(pos > 1)
        out[pos++] = '/';
      if(pos + len >= out_len)
        return -1;
      memcpy(out + pos, src, (unsigned)len);
      pos += len;
    }
    out[pos] = 0;
  }
  return 0;
}

static void vfs_lock(void)
{
  if(g_vfs_lock)
    (void)xSemaphoreTake(g_vfs_lock, portMAX_DELAY);
}

static void vfs_unlock(void)
{
  if(g_vfs_lock)
    (void)xSemaphoreGive(g_vfs_lock);
}

static int dev_canonical_path(const char *path, char *out, int out_len)
{
  if(path == 0 || out == 0 || out_len <= 0)
    return -1;
  if(strcmp(path, "/dev/fd/0") == 0)
    path = "/dev/stdin";
  else if(strcmp(path, "/dev/fd/1") == 0)
    path = "/dev/stdout";
  else if(strcmp(path, "/dev/fd/2") == 0)
    path = "/dev/stderr";
  else if(strcmp(path, "/dev/pts/ptmx") == 0)
    path = "/dev/ptmx";
  if((int)strlen(path) >= out_len)
    return -1;
  copy_cstr(out, out_len, path);
  return 0;
}

static int parse_pts_id(const char *path, int *out_id);

static int is_known_dev_canon(const char *canon)
{
  int pty_id = -1;

  if(canon == 0)
    return 0;
  if(strcmp(canon, "/dev") == 0 || strcmp(canon, "/dev/console") == 0 || strcmp(canon, "/dev/tty") == 0 ||
     strcmp(canon, "/dev/null") == 0 || strcmp(canon, "/dev/zero") == 0 || strcmp(canon, "/dev/full") == 0 ||
     strcmp(canon, "/dev/random") == 0 || strcmp(canon, "/dev/urandom") == 0 || strcmp(canon, "/dev/stdin") == 0 ||
     strcmp(canon, "/dev/stdout") == 0 || strcmp(canon, "/dev/stderr") == 0 || strcmp(canon, "/dev/kmsg") == 0 ||
     strcmp(canon, "/dev/ptmx") == 0 || strcmp(canon, "/dev/pts") == 0)
    return 1;
  if(parse_pts_id(canon, &pty_id) == 0)
    return 1;
  return 0;
}

static int is_dev_node(const char *path)
{
  char canon[VFS_PATH_MAX];
  if(dev_canonical_path(path, canon, sizeof(canon)) != 0)
    return 0;
  return is_known_dev_canon(canon);
}

static int parse_pts_id(const char *path, int *out_id)
{
  const char *p;
  int v = 0;
  if(path == 0 || out_id == 0)
    return -1;
  if(strncmp(path, "/dev/pts/", 9) != 0)
    return -1;
  p = path + 9;
  if(*p < '0' || *p > '9')
    return -1;
  while(*p >= '0' && *p <= '9'){
    v = v * 10 + (*p - '0');
    if(v >= XV6_MAX_PTY)
      return -1;
    p++;
  }
  if(*p != 0)
    return -1;
  *out_id = v;
  return 0;
}

static int pty_q_push(uint8 *buf, uint16 *w, uint16 *n, uint16 cap, const uint8 *src, uint32 size)
{
  uint32 i;
  uint32 wrote = 0;
  for(i = 0; i < size; i++){
    if(*n >= cap)
      break;
    buf[*w] = src[i];
    *w = (uint16)((*w + 1) % cap);
    (*n)++;
    wrote++;
  }
  return (int)wrote;
}

static int pty_q_pop(uint8 *buf, uint16 *r, uint16 *n, uint16 cap, uint8 *dst, uint32 size)
{
  uint32 i;
  uint32 out = 0;
  for(i = 0; i < size; i++){
    if(*n == 0)
      break;
    dst[i] = buf[*r];
    *r = (uint16)((*r + 1) % cap);
    (*n)--;
    out++;
  }
  return (int)out;
}

static void pty_q_drop_all(uint16 *r, uint16 *w, uint16 *n)
{
  if(r == 0 || w == 0 || n == 0)
    return;
  *r = 0;
  *w = 0;
  *n = 0;
}

static int pty_alloc_id(void)
{
  int i;
  for(i = 0; i < XV6_MAX_PTY; i++){
    if(!g_ptys[i].alloc){
      uint8 *m2s = (uint8 *)vfs_alloc_data(XV6_PTY_BUF_CAP);
      uint8 *s2m;
      if(m2s == 0)
        return -1;
      s2m = (uint8 *)vfs_alloc_data(XV6_PTY_BUF_CAP);
      if(s2m == 0){
        heap_caps_free(m2s);
        return -1;
      }
      pty_slot_reset(&g_ptys[i]);
      g_ptys[i].m2s = m2s;
      g_ptys[i].s2m = s2m;
      g_ptys[i].alloc = 1;
      return i;
    }
  }
  return -1;
}

static void pty_try_free(int id)
{
  if(id < 0 || id >= XV6_MAX_PTY)
    return;
  if(g_ptys[id].alloc && !g_ptys[id].master_open && !g_ptys[id].slave_open && g_ptys[id].m2s_n == 0 &&
     g_ptys[id].s2m_n == 0)
    pty_slot_reset(&g_ptys[id]);
}

static void pty_drop_queued_data_if_orphaned(int id)
{
  if(id < 0 || id >= XV6_MAX_PTY || !g_ptys[id].alloc)
    return;
  if(g_ptys[id].master_open == 0 && g_ptys[id].slave_open == 0){
    g_ptys[id].m2s_r = g_ptys[id].m2s_w = g_ptys[id].m2s_n = 0;
    g_ptys[id].s2m_r = g_ptys[id].s2m_w = g_ptys[id].s2m_n = 0;
  }
}

static int pipe_alloc_id(void)
{
  int i;
  for(i = 0; i < XV6_MAX_PIPE; i++){
    if(!g_pipes[i].alloc){
      uint8 *buf = (uint8 *)vfs_alloc_data(XV6_PIPE_BUF_CAP);
      if(buf == 0)
        return -1;
      pipe_slot_reset(&g_pipes[i]);
      g_pipes[i].data = buf;
      g_pipes[i].alloc = 1;
      return i;
    }
  }
  return -1;
}

static void pipe_try_free(int id)
{
  if(id < 0 || id >= XV6_MAX_PIPE)
    return;
  if(g_pipes[id].alloc && g_pipes[id].readers == 0 && g_pipes[id].writers == 0)
    pipe_slot_reset(&g_pipes[id]);
}

static int pipe_endpoints_from_flags(int flags, int *readers, int *writers)
{
  int acc_mode = (flags & XV6_O_ACCMODE);

  if(readers == 0 || writers == 0)
    return -1;
  *readers = 0;
  *writers = 0;
  if(acc_mode == XV6_O_RDONLY){
    *readers = 1;
    return 0;
  }
  if(acc_mode == XV6_O_WRONLY){
    *writers = 1;
    return 0;
  }
  if(acc_mode == XV6_O_RDWR){
    *readers = 1;
    *writers = 1;
    return 0;
  }
  return -1;
}

static void fifo_unbind_pipe_locked(int pipe_id)
{
  int i;

  if(pipe_id < 0 || pipe_id >= XV6_MAX_PIPE)
    return;
  for(i = 0; i < XV6_MAX_PIPE; i++){
    if(g_fifo_links[i].used && g_fifo_links[i].pipe_id == pipe_id){
      memset(&g_fifo_links[i], 0, sizeof(g_fifo_links[i]));
      return;
    }
  }
}

static int fifo_bind_or_get_pipe_locked(uint32 inum)
{
  int i;
  int free_idx = -1;
  int pipe_id;

  if(inum == 0)
    return -1;
  for(i = 0; i < XV6_MAX_PIPE; i++){
    if(!g_fifo_links[i].used){
      if(free_idx < 0)
        free_idx = i;
      continue;
    }
    if(g_fifo_links[i].inum != inum)
      continue;
    pipe_id = g_fifo_links[i].pipe_id;
    if(pipe_id >= 0 && pipe_id < XV6_MAX_PIPE && g_pipes[pipe_id].alloc)
      return pipe_id;
    memset(&g_fifo_links[i], 0, sizeof(g_fifo_links[i]));
    free_idx = i;
    break;
  }

  pipe_id = pipe_alloc_id();
  if(pipe_id < 0)
    return -1;
  if(free_idx < 0){
    pipe_slot_reset(&g_pipes[pipe_id]);
    return -1;
  }

  g_fifo_links[free_idx].used = 1;
  g_fifo_links[free_idx].inum = inum;
  g_fifo_links[free_idx].pipe_id = pipe_id;
  return pipe_id;
}

static int dev_prng_fill(void *buf, uint32 n)
{
  uint8 *p = (uint8 *)buf;
  uint32 x = (uint32)hal_ticks() ^ 0x9e3779b9u;
  uint32 i;

  for(i = 0; i < n; i++){
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    p[i] = (uint8)x;
  }
  return (int)n;
}

static int dev_read(const char *path, uint32 off, void *buf, uint32 size)
{
  char canon[VFS_PATH_MAX];
  uint8 *p = (uint8 *)buf;
  uint32 i = 0;

  if(dev_canonical_path(path, canon, sizeof(canon)) != 0 || buf == 0)
    return -1;
  (void)off;

  if(strcmp(canon, "/dev/null") == 0)
    return 0;
  if(strcmp(canon, "/dev/zero") == 0 || strcmp(canon, "/dev/full") == 0){
    memset(buf, 0, size);
    return (int)size;
  }
  if(strcmp(canon, "/dev/random") == 0 || strcmp(canon, "/dev/urandom") == 0)
    return dev_prng_fill(buf, size);
  if(strcmp(canon, "/dev/stdin") == 0 || strcmp(canon, "/dev/tty") == 0){
    while(i < size){
      int c = hal_console_getc();
      if(c < 0){
        if(i > 0)
          break;
        hal_delay_ms(1);
        continue;
      }
      {
        int sig = hostabi_posix_tty_signal_for_char(c);
        if(sig != 0){
          (void)hostabi_posix_tty_dispatch_signal(sig);
          continue;
        }
      }
      p[i++] = (uint8)c;
      if(c == '\n' || c == '\r')
        break;
    }
    return (int)i;
  }

  return -1;
}

static int dev_write(const char *path, const void *data, uint32 size)
{
  char canon[VFS_PATH_MAX];
  const char *c = (const char *)data;
  uint32 left = size;

  if(dev_canonical_path(path, canon, sizeof(canon)) != 0 || data == 0)
    return -1;
  if(strcmp(canon, "/dev/full") == 0 || strcmp(canon, "/dev/stdin") == 0)
    return -1;

  if(strcmp(canon, "/dev/console") == 0 || strcmp(canon, "/dev/tty") == 0 || strcmp(canon, "/dev/stdout") == 0 ||
     strcmp(canon, "/dev/stderr") == 0 || strcmp(canon, "/dev/kmsg") == 0){
    while(left--)
      hal_console_putc(*c++);
    return (int)size;
  }

  if(strcmp(canon, "/dev/null") == 0 || strcmp(canon, "/dev/zero") == 0 || strcmp(canon, "/dev/random") == 0 ||
     strcmp(canon, "/dev/urandom") == 0)
    return (int)size;
  return -1;
}

static int dev_read_fd(const xv6_vfd_t *fd, void *buf, uint32 size)
{
  if(fd == 0 || buf == 0)
    return -1;
  if(fd->dev_role == DEV_ROLE_PTY_MASTER || fd->dev_role == DEV_ROLE_PTY_SLAVE){
    int id = fd->dev_id;
    xv6_pty_t *p;
    if(id < 0 || id >= XV6_MAX_PTY || !g_ptys[id].alloc)
      return -1;
    p = &g_ptys[id];
    if(p->m2s == 0 || p->s2m == 0)
      return -1;
    if(fd->dev_role == DEV_ROLE_PTY_MASTER)
      return pty_q_pop(p->s2m, &p->s2m_r, &p->s2m_n, (uint16)XV6_PTY_BUF_CAP, (uint8 *)buf, size);
    return pty_q_pop(p->m2s, &p->m2s_r, &p->m2s_n, (uint16)XV6_PTY_BUF_CAP, (uint8 *)buf, size);
  }
  return dev_read(fd->path, fd->off, buf, size);
}

static int dev_write_fd(const xv6_vfd_t *fd, const void *buf, uint32 size)
{
  if(fd == 0 || buf == 0)
    return -1;
  if(fd->dev_role == DEV_ROLE_PTY_MASTER || fd->dev_role == DEV_ROLE_PTY_SLAVE){
    int id = fd->dev_id;
    xv6_pty_t *p;
    if(id < 0 || id >= XV6_MAX_PTY || !g_ptys[id].alloc)
      return -1;
    p = &g_ptys[id];
    if(p->m2s == 0 || p->s2m == 0)
      return -1;
    if(fd->dev_role == DEV_ROLE_PTY_MASTER)
      return pty_q_push(p->m2s, &p->m2s_w, &p->m2s_n, (uint16)XV6_PTY_BUF_CAP, (const uint8 *)buf, size);
    return pty_q_push(p->s2m, &p->s2m_w, &p->s2m_n, (uint16)XV6_PTY_BUF_CAP, (const uint8 *)buf, size);
  }
  return dev_write(fd->path, buf, size);
}

static int dev_read_alloc(const char *path, void **out_data, uint32 *out_size)
{
  uint8 *buf;
  int n;
  if(path == 0 || out_data == 0 || out_size == 0)
    return -1;
  *out_size = 256;
  buf = (uint8 *)malloc(*out_size);
  if(buf == 0)
    return -1;
  n = dev_read(path, 0, buf, *out_size);
  if(n < 0){
    free(buf);
    return -1;
  }
  *out_size = (uint32)n;
  *out_data = buf;
  return 0;
}

static int read_block(uint32 bno, void *dst)
{
  return esp_flash_disk_read(bno * (BSIZE / XV6_FLASH_SECTOR_SIZE), dst,
                             (BSIZE / XV6_FLASH_SECTOR_SIZE));
}

static int write_block(uint32 bno, const void *src)
{
  return esp_flash_disk_write(bno * (BSIZE / XV6_FLASH_SECTOR_SIZE), src,
                              (BSIZE / XV6_FLASH_SECTOR_SIZE));
}

static int read_inode(uint32 inum, struct dinode *out)
{
  uint8 blk[BSIZE];
  uint32 bn;
  struct dinode *dip;

  if(out == 0 || !g_ready || inum == 0)
    return -1;
  bn = IBLOCK(inum, g_sb);
  if(read_block(bn, blk) != 0)
    return -1;
  dip = ((struct dinode *)blk) + (inum % IPB);
  *out = *dip;
  return 0;
}

static int write_inode(uint32 inum, const struct dinode *in)
{
  uint8 blk[BSIZE];
  uint32 bn;
  struct dinode *dip;

  if(in == 0 || !g_ready || inum == 0)
    return -1;
  bn = IBLOCK(inum, g_sb);
  if(read_block(bn, blk) != 0)
    return -1;
  dip = ((struct dinode *)blk) + (inum % IPB);
  *dip = *in;
  return write_block(bn, blk);
}

static int inode_data_block(const struct dinode *ip, uint32 fbn, uint32 *out_bno)
{
  uint8 iblk[BSIZE];
  uint32 *indirect = (uint32 *)iblk;

  if(ip == 0 || out_bno == 0)
    return -1;
  if(fbn < NDIRECT){
    *out_bno = ip->addrs[fbn];
    return (*out_bno != 0) ? 0 : -1;
  }
  fbn -= NDIRECT;
  if(fbn >= NINDIRECT || ip->addrs[NDIRECT] == 0)
    return -1;
  if(read_block(ip->addrs[NDIRECT], iblk) != 0)
    return -1;
  *out_bno = indirect[fbn];
  return (*out_bno != 0) ? 0 : -1;
}

static int inode_read_range(const struct dinode *ip, uint32 off, void *dst, uint32 n)
{
  uint8 blk[BSIZE];
  uint8 *out = (uint8 *)dst;
  uint32 copied = 0;
  uint32 inode_sz;

  if(ip == 0 || dst == 0)
    return -1;
  inode_sz = inode_get_size(ip);
  if(off > inode_sz)
    return -1;
  if(n > inode_sz - off)
    return -1;

  while(copied < n){
    uint32 file_off = off + copied;
    uint32 fbn = file_off / BSIZE;
    uint32 boff = file_off % BSIZE;
    uint32 take = BSIZE - boff;
    uint32 dblk;

    if(take > (n - copied))
      take = n - copied;
    if(inode_data_block(ip, fbn, &dblk) != 0)
      return -1;
    if(read_block(dblk, blk) != 0)
      return -1;
    memcpy(out + copied, blk + boff, take);
    copied += take;
  }
  return 0;
}

static int bitmap_get(uint32 bno, int *used)
{
  uint8 buf[BSIZE];
  uint32 bb = BBLOCK(bno, g_sb);
  uint32 bi = bno % BPB;
  if(read_block(bb, buf) != 0)
    return -1;
  *used = ((buf[bi / 8] >> (bi % 8)) & 1);
  return 0;
}

static int bitmap_set(uint32 bno, int used)
{
  uint8 buf[BSIZE];
  uint32 bb = BBLOCK(bno, g_sb);
  uint32 bi = bno % BPB;
  if(read_block(bb, buf) != 0)
    return -1;
  if(used)
    buf[bi / 8] |= (1u << (bi % 8));
  else
    buf[bi / 8] &= ~(1u << (bi % 8));
  return write_block(bb, buf);
}

static int alloc_block(uint32 *out_bno)
{
  uint32 b;
  for(b = g_data_start; b < g_sb.size; b++){
    int used = 0;
    if(bitmap_get(b, &used) != 0)
      return -1;
    if(!used){
      uint8 zero[BSIZE];
      memset(zero, 0, sizeof(zero));
      if(bitmap_set(b, 1) != 0)
        return -1;
      if(write_block(b, zero) != 0)
        return -1;
      *out_bno = b;
      return 0;
    }
  }
  return -1;
}

static int free_block(uint32 bno)
{
  return bitmap_set(bno, 0);
}

static int alloc_inode(short type, uint32 *out_inum)
{
  uint32 i;
  for(i = 1; i < g_sb.ninodes; i++){
    struct dinode ip;
    if(read_inode(i, &ip) != 0)
      return -1;
    if(ip.type == 0){
      memset(&ip, 0, sizeof(ip));
      ip.type = type;
      ip.nlink = 1;
      inode_set_size(&ip, 0);
      inode_init_attrs(&ip, type);
      if(write_inode(i, &ip) != 0)
        return -1;
      *out_inum = i;
      return 0;
    }
  }
  return -1;
}

static int inode_get_or_alloc_block(struct dinode *ip, uint32 fbn, int alloc, uint32 *out_bno)
{
  uint8 iblk[BSIZE];
  uint32 *indirect = (uint32 *)iblk;

  if(fbn < NDIRECT){
    if(ip->addrs[fbn] == 0 && alloc){
      if(alloc_block(&ip->addrs[fbn]) != 0)
        return -1;
    }
    *out_bno = ip->addrs[fbn];
    return (*out_bno != 0) ? 0 : -1;
  }

  fbn -= NDIRECT;
  if(fbn >= NINDIRECT)
    return -1;
  if(ip->addrs[NDIRECT] == 0){
    if(!alloc)
      return -1;
    if(alloc_block(&ip->addrs[NDIRECT]) != 0)
      return -1;
    memset(iblk, 0, sizeof(iblk));
    if(write_block(ip->addrs[NDIRECT], iblk) != 0)
      return -1;
  }
  if(read_block(ip->addrs[NDIRECT], iblk) != 0)
    return -1;
  if(indirect[fbn] == 0 && alloc){
    if(alloc_block(&indirect[fbn]) != 0)
      return -1;
    if(write_block(ip->addrs[NDIRECT], iblk) != 0)
      return -1;
  }
  *out_bno = indirect[fbn];
  return (*out_bno != 0) ? 0 : -1;
}

static int inode_write_range(struct dinode *ip, uint32 off, const void *src, uint32 n)
{
  uint8 blk[BSIZE];
  const uint8 *in = (const uint8 *)src;
  uint32 copied = 0;
  uint32 end_off;
  uint32 max_file_size = (uint32)(NDIRECT + NINDIRECT) * BSIZE;
  uint32 inode_sz;

  if(ip == 0 || src == 0)
    return -1;
  if(n == 0)
    return 0;
  if(off > (uint32)0xffffffffu - n)
    return -1;
  end_off = off + n;
  if(end_off > max_file_size)
    return -1;
  inode_sz = inode_get_size(ip);

  while(copied < n){
    uint32 file_off = off + copied;
    uint32 fbn = file_off / BSIZE;
    uint32 boff = file_off % BSIZE;
    uint32 take = BSIZE - boff;
    uint32 dblk;

    if(take > (n - copied))
      take = n - copied;
    if(inode_get_or_alloc_block(ip, fbn, 1, &dblk) != 0)
      return -1;
    if(read_block(dblk, blk) != 0)
      return -1;
    memcpy(blk + boff, in + copied, take);
    if(write_block(dblk, blk) != 0)
      return -1;
    copied += take;
  }
  if(end_off > inode_sz)
    inode_set_size(ip, end_off);
  return 0;
}

static int inode_truncate(uint32 inum, struct dinode *ip)
{
  uint32 i;
  uint8 iblk[BSIZE];
  uint32 *indirect = (uint32 *)iblk;

  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      if(free_block(ip->addrs[i]) != 0)
        return -1;
      ip->addrs[i] = 0;
    }
  }
  if(ip->addrs[NDIRECT]){
    if(read_block(ip->addrs[NDIRECT], iblk) != 0)
      return -1;
    for(i = 0; i < NINDIRECT; i++){
      if(indirect[i] && free_block(indirect[i]) != 0)
        return -1;
    }
    if(free_block(ip->addrs[NDIRECT]) != 0)
      return -1;
    ip->addrs[NDIRECT] = 0;
  }
  inode_set_size(ip, 0);
  return write_inode(inum, ip);
}

static uint32 inode_max_file_size(void)
{
  return (uint32)(NDIRECT + NINDIRECT) * BSIZE;
}

static int inode_resize(uint32 inum, struct dinode *ip, uint32 new_size)
{
  uint32 old_size;
  uint32 keep_fbn;
  uint32 i;
  int ind_modified;
  int ind_has_entries;
  static const uint8 zero_blk[BSIZE] = { 0 };

  if(ip == 0)
    return -1;
  old_size = inode_get_size(ip);
  if(new_size == old_size)
    return 0;
  if(new_size > inode_max_file_size())
    return -1;

  if(new_size == 0)
    return inode_truncate(inum, ip);

  if(new_size > old_size){
    uint32 off = old_size;
    while(off < new_size){
      uint32 take = new_size - off;
      if(take > BSIZE)
        take = BSIZE;
      if(inode_write_range(ip, off, zero_blk, take) != 0)
        return -1;
      off += take;
    }
    return write_inode(inum, ip);
  }

  keep_fbn = (new_size + (BSIZE - 1u)) / BSIZE;
  if((new_size % BSIZE) != 0u){
    uint32 tail_fbn = new_size / BSIZE;
    uint32 tail_bno = 0;
    uint32 tail_off = new_size % BSIZE;
    uint8 blk[BSIZE];

    if(inode_get_or_alloc_block(ip, tail_fbn, 0, &tail_bno) == 0){
      if(read_block(tail_bno, blk) != 0)
        return -1;
      memset(blk + tail_off, 0, BSIZE - tail_off);
      if(write_block(tail_bno, blk) != 0)
        return -1;
    }
  }

  for(i = keep_fbn; i < NDIRECT; i++){
    if(ip->addrs[i] == 0)
      continue;
    if(free_block(ip->addrs[i]) != 0)
      return -1;
    ip->addrs[i] = 0;
  }

  if(ip->addrs[NDIRECT] != 0){
    uint8 iblk[BSIZE];
    uint32 *indirect = (uint32 *)iblk;

    if(read_block(ip->addrs[NDIRECT], iblk) != 0)
      return -1;

    ind_modified = 0;
    for(i = 0; i < NINDIRECT; i++){
      uint32 fbn = NDIRECT + i;
      if(fbn < keep_fbn)
        continue;
      if(indirect[i] == 0)
        continue;
      if(free_block(indirect[i]) != 0)
        return -1;
      indirect[i] = 0;
      ind_modified = 1;
    }

    ind_has_entries = 0;
    for(i = 0; i < NINDIRECT; i++){
      if(indirect[i] != 0){
        ind_has_entries = 1;
        break;
      }
    }

    if(!ind_has_entries){
      if(free_block(ip->addrs[NDIRECT]) != 0)
        return -1;
      ip->addrs[NDIRECT] = 0;
    } else if(ind_modified){
      if(write_block(ip->addrs[NDIRECT], iblk) != 0)
        return -1;
    }
  }

  inode_set_size(ip, new_size);
  return write_inode(inum, ip);
}

static void inode_reclaim_orphan_locked(uint32 inum)
{
  struct dinode ip;

  if(inum == 0)
    return;
  if(read_inode(inum, &ip) != 0 || ip.type == 0)
    return;
  (void)inode_truncate(inum, &ip);
  memset(&ip, 0, sizeof(ip));
  (void)write_inode(inum, &ip);
}

static int next_path_elem(const char **pp, char *name)
{
  const char *p = *pp;
  int n = 0;

  while(*p == '/')
    p++;
  if(*p == 0){
    *pp = p;
    return 0;
  }
  while(*p && *p != '/'){
    if(n < DIRSIZ)
      name[n++] = *p;
    p++;
  }
  name[n] = 0;
  while(*p == '/')
    p++;
  *pp = p;
  return 1;
}

static int dir_lookup_inum(uint32 dir_inum, const char *name, uint32 *out_inum, struct dinode *out_ip)
{
  struct dinode dir;
  uint32 off;
  uint32 dir_sz;

  if(read_inode(dir_inum, &dir) != 0 || dir.type != T_DIR)
    return -1;
  dir_sz = inode_get_size(&dir);

  for(off = 0; off + sizeof(struct dirent) <= dir_sz; off += sizeof(struct dirent)){
    struct dirent de;
    char dname[DIRSIZ + 1];
    if(inode_read_range(&dir, off, &de, sizeof(de)) != 0)
      return -1;
    if(de.inum == 0)
      continue;
    memset(dname, 0, sizeof(dname));
    memcpy(dname, de.name, DIRSIZ);
    if(strcmp(name, dname) == 0){
      if(out_inum)
        *out_inum = de.inum;
      if(out_ip && read_inode(de.inum, out_ip) != 0)
        return -1;
      return 0;
    }
  }
  return 1;
}

static int path_lookup(const char *path, uint32 *out_inum, struct dinode *out_ip)
{
  const char *p = path;
  uint32 inum = ROOTINO;
  struct dinode ip;
  char elem[DIRSIZ + 1];

  if(path == 0 || path[0] == 0)
    return -1;
  if(read_inode(ROOTINO, &ip) != 0)
    return -1;
  if(strcmp(path, "/") == 0){
    if(out_inum)
      *out_inum = ROOTINO;
    if(out_ip)
      *out_ip = ip;
    return 0;
  }

  while(next_path_elem(&p, elem)){
    uint32 next_inum = 0;
    struct dinode next_ip;
    int rc = dir_lookup_inum(inum, elem, &next_inum, &next_ip);
    if(rc != 0)
      return rc;
    inum = next_inum;
    ip = next_ip;
  }

  if(out_inum)
    *out_inum = inum;
  if(out_ip)
    *out_ip = ip;
  return 0;
}

static int path_parent(const char *path, uint32 *parent_inum, char *name_out)
{
  const char *p = path;
  uint32 inum = ROOTINO;
  struct dinode ip;
  char elem[DIRSIZ + 1];
  char next[DIRSIZ + 1];

  if(path == 0 || path[0] != '/')
    return -1;
  if(read_inode(ROOTINO, &ip) != 0)
    return -1;

  if(!next_path_elem(&p, elem))
    return -1;
  while(1){
    const char *save = p;
    if(!next_path_elem(&save, next)){
      if(parent_inum)
        *parent_inum = inum;
      copy_cstr(name_out, DIRSIZ + 1, elem);
      return 0;
    }
    {
      uint32 next_inum = 0;
      struct dinode next_ip;
      if(dir_lookup_inum(inum, elem, &next_inum, &next_ip) != 0 || next_ip.type != T_DIR)
        return -1;
      inum = next_inum;
      ip = next_ip;
      (void)ip;
      p = save;
      copy_cstr(elem, sizeof(elem), next);
    }
  }
}

static int path_parent_str(const char *path, char *out, int out_len)
{
  const char *slash;
  int len;

  if(path == 0 || out == 0 || out_len <= 1 || path[0] != '/')
    return -1;
  if(strcmp(path, "/") == 0)
    return -1;

  slash = strrchr(path, '/');
  if(slash == 0)
    return -1;
  if(slash == path){
    if(out_len < 2)
      return -1;
    out[0] = '/';
    out[1] = 0;
    return 0;
  }

  len = (int)(slash - path);
  if(len <= 0 || len >= out_len)
    return -1;
  memcpy(out, path, (size_t)len);
  out[len] = 0;
  return 0;
}

static int inode_read_symlink_target_locked(const struct dinode *ip, char *out, int out_len, uint32 *out_n)
{
  uint32 n;

  if(ip == 0 || out == 0 || out_len <= 1 || ip->type != T_SYMLINK)
    return -1;
  n = inode_get_size(ip);
  if(n >= (uint32)out_len)
    return -1;
  if(n > 0 && inode_read_range(ip, 0, out, n) != 0)
    return -1;
  out[n] = 0;
  if(out_n)
    *out_n = n;
  return 0;
}

static int path_resolve_final_symlink_locked_impl(const char *abs_in, char *abs_out, int out_len,
                                                  int allow_missing_final_nonzero)
{
  char cur[VFS_PATH_MAX];
  int depth;

  if(abs_in == 0 || abs_out == 0 || out_len <= 1)
    return -1;
  copy_cstr(cur, sizeof(cur), abs_in);

  for(depth = 0; depth < XV6_SYMLINK_MAX_DEPTH; depth++){
    uint32 inum = 0;
    struct dinode ip;

    if(is_dev_node(cur)){
      if((int)strlen(cur) >= out_len)
        return -1;
      copy_cstr(abs_out, out_len, cur);
      return 0;
    }

    {
      int rc = path_lookup(cur, &inum, &ip);
      if(rc != 0){
        if(rc == 1 && allow_missing_final_nonzero){
          if((int)strlen(cur) >= out_len)
            return -1;
          copy_cstr(abs_out, out_len, cur);
          return 0;
        }
        return -1;
      }
    }
    if(ip.type != T_SYMLINK){
      if((int)strlen(cur) >= out_len)
        return -1;
      copy_cstr(abs_out, out_len, cur);
      return 0;
    }

    {
      char target[XV6_SYMLINK_TARGET_MAX];
      char combined[VFS_PATH_MAX];

      if(inode_read_symlink_target_locked(&ip, target, sizeof(target), 0) != 0)
        return -1;
      if(target[0] == '/'){
        copy_cstr(combined, sizeof(combined), target);
      } else {
        char parent[VFS_PATH_MAX];
        int n;
        if(path_parent_str(cur, parent, sizeof(parent)) != 0)
          return -1;
        n = snprintf(combined, sizeof(combined), "%s/%s", parent, target);
        if(n <= 0 || n >= (int)sizeof(combined))
          return -1;
      }
      if(path_resolve(combined, cur, sizeof(cur)) != 0)
        return -1;
    }
  }

  task_ctx_set_errno(ELOOP);
  return -1;
}

static int path_resolve_final_symlink_locked(const char *abs_in, char *abs_out, int out_len)
{
  return path_resolve_final_symlink_locked_impl(abs_in, abs_out, out_len, 0);
}

static int path_lookup_follow_locked(const char *abs_path, int follow_final_nonzero, char *resolved_out, int resolved_len,
                                     uint32 *out_inum, struct dinode *out_ip)
{
  char lookup_path[VFS_PATH_MAX];
  const char *path_for_lookup = abs_path;

  if(abs_path == 0)
    return -1;

  if(follow_final_nonzero){
    if(path_resolve_final_symlink_locked(abs_path, lookup_path, sizeof(lookup_path)) != 0)
      return -1;
    path_for_lookup = lookup_path;
  }

  if(path_lookup(path_for_lookup, out_inum, out_ip) != 0)
    return -1;

  if(resolved_out){
    if(resolved_len <= 1 || (int)strlen(path_for_lookup) >= resolved_len)
      return -1;
    copy_cstr(resolved_out, resolved_len, path_for_lookup);
  }

  return 0;
}

static int dir_add_entry(uint32 dir_inum, const char *name, uint32 inum)
{
  struct dinode dir;
  uint32 off;
  struct dirent de;
  uint32 dir_sz;

  if(strlen(name) > DIRSIZ)
    return -1;
  if(read_inode(dir_inum, &dir) != 0 || dir.type != T_DIR)
    return -1;
  dir_sz = inode_get_size(&dir);

  for(off = 0; off + sizeof(de) <= dir_sz; off += sizeof(de)){
    if(inode_read_range(&dir, off, &de, sizeof(de)) != 0)
      return -1;
    if(de.inum == 0)
      break;
  }

  memset(&de, 0, sizeof(de));
  de.inum = inum;
  memcpy(de.name, name, strlen(name));
  if(inode_write_range(&dir, off, &de, sizeof(de)) != 0)
    return -1;
  return write_inode(dir_inum, &dir);
}

static int dir_find_entry_offset(uint32 dir_inum, const char *name, uint32 *out_off, struct dirent *out_de)
{
  struct dinode dir;
  uint32 off;
  uint32 dir_sz;

  if(read_inode(dir_inum, &dir) != 0 || dir.type != T_DIR)
    return -1;
  dir_sz = inode_get_size(&dir);

  for(off = 0; off + sizeof(struct dirent) <= dir_sz; off += sizeof(struct dirent)){
    struct dirent de;
    char dname[DIRSIZ + 1];
    if(inode_read_range(&dir, off, &de, sizeof(de)) != 0)
      return -1;
    if(de.inum == 0)
      continue;
    memset(dname, 0, sizeof(dname));
    memcpy(dname, de.name, DIRSIZ);
    if(strcmp(name, dname) == 0){
      if(out_off)
        *out_off = off;
      if(out_de)
        *out_de = de;
      return 0;
    }
  }
  return 1;
}

static int dir_replace_entry_at(uint32 dir_inum, uint32 off, const struct dirent *in_de)
{
  struct dinode dir;
  uint32 dir_sz;

  if(in_de == 0)
    return -1;
  if(read_inode(dir_inum, &dir) != 0 || dir.type != T_DIR)
    return -1;
  dir_sz = inode_get_size(&dir);
  if(off + sizeof(*in_de) > dir_sz)
    return -1;
  if(inode_write_range(&dir, off, in_de, sizeof(*in_de)) != 0)
    return -1;
  return write_inode(dir_inum, &dir);
}

static int dir_clear_entry_at(uint32 dir_inum, uint32 off)
{
  struct dirent de;
  memset(&de, 0, sizeof(de));
  return dir_replace_entry_at(dir_inum, off, &de);
}

static int dir_is_empty(uint32 dir_inum)
{
  struct dinode dir;
  uint32 off;
  uint32 dir_sz;

  if(read_inode(dir_inum, &dir) != 0 || dir.type != T_DIR)
    return -1;
  dir_sz = inode_get_size(&dir);
  for(off = 0; off + sizeof(struct dirent) <= dir_sz; off += sizeof(struct dirent)){
    struct dirent de;
    char dname[DIRSIZ + 1];
    if(inode_read_range(&dir, off, &de, sizeof(de)) != 0)
      return -1;
    if(de.inum == 0)
      continue;
    memset(dname, 0, sizeof(dname));
    memcpy(dname, de.name, DIRSIZ);
    if(strcmp(dname, ".") == 0 || strcmp(dname, "..") == 0)
      continue;
    return 0;
  }
  return 1;
}

static int dir_set_dotdot(uint32 dir_inum, uint32 parent_inum)
{
  struct dirent de;
  uint32 off = 0;

  if(dir_find_entry_offset(dir_inum, "..", &off, &de) != 0)
    return -1;
  memset(&de, 0, sizeof(de));
  de.inum = parent_inum;
  memcpy(de.name, "..", 2);
  return dir_replace_entry_at(dir_inum, off, &de);
}

static int inode_drop_link_locked(uint32 inum, struct dinode *ip)
{
  if(ip == 0)
    return -1;
  if(ip->nlink > 0)
    ip->nlink--;
  if(ip->nlink == 0){
    if(inode_truncate(inum, ip) != 0)
      return -1;
    memset(ip, 0, sizeof(*ip));
  }
  return write_inode(inum, ip);
}

static int inode_add_link_locked(uint32 inum, struct dinode *ip)
{
  if(ip == 0)
    return -1;
  if(ip->nlink < 0 || ip->nlink >= 0x7fff)
    return -1;
  ip->nlink++;
  return write_inode(inum, ip);
}

int xv6fs_ro_init(void)
{
  uint8 blk[BSIZE];

  g_ready = 0;
  if(g_vfs_lock == 0)
    g_vfs_lock = xSemaphoreCreateMutex();
  if(g_ctx_lock == 0)
    g_ctx_lock = xSemaphoreCreateMutex();
  if(g_vfs_lock == 0 || g_ctx_lock == 0){
    ESP_LOGE(TAG, "mutex init failed");
    return -1;
  }
  memset(&g_sb, 0, sizeof(g_sb));

  if(read_block(1, blk) != 0)
    return -1;
  memcpy(&g_sb, blk, sizeof(g_sb));
  if(g_sb.magic != FSMAGIC){
    ESP_LOGE(TAG, "bad magic: 0x%08x", g_sb.magic);
    return -1;
  }

  g_nbitmap = g_sb.size / BPB + 1;
  g_data_start = g_sb.bmapstart + g_nbitmap;
  xv6_vfs_reset();
  if(g_fds == 0){
    ESP_LOGE(TAG, "fd table init failed");
    return -1;
  }
  g_ready = 1;
  ESP_LOGI(TAG, "mounted: size=%u nblocks=%u ninodes=%u", g_sb.size, g_sb.nblocks, g_sb.ninodes);
  return 0;
}

int xv6fs_ro_flash_image(const uint8 *image, uint32 image_size)
{
  uint32 sectors;
  if(image == 0 || image_size == 0 || (image_size % XV6_FLASH_SECTOR_SIZE) != 0)
    return -1;
  sectors = image_size / XV6_FLASH_SECTOR_SIZE;
  if(sectors > esp_flash_disk_num_sectors())
    return -1;
  return esp_flash_disk_write(0, image, sectors);
}

int xv6fs_list_path(const char *path, int index, char *name_out, int name_out_len, uint16 *type_out, uint32 *size_out)
{
  char abs_path[VFS_PATH_MAX];
  uint32 dir_inum;
  struct dinode dir;
  uint32 off;
  uint32 dir_sz;
  int seen = 0;
  int rc = -1;
  int err = ENOENT;

  task_ctx_clear_errno();
  if(index < 0 || name_out == 0 || name_out_len <= 1){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(!g_ready){
    task_ctx_set_errno(ENODEV);
    return -1;
  }
  if(path_resolve(path, abs_path, sizeof(abs_path)) != 0){
    task_ctx_set_errno(ENOENT);
    return -1;
  }
  vfs_lock();
  if(path_lookup(abs_path, &dir_inum, &dir) != 0){
    err = ENOENT;
    goto out;
  }
  if(dir.type != T_DIR){
    err = ENOTDIR;
    goto out;
  }
  dir_sz = inode_get_size(&dir);

  for(off = 0; off + sizeof(struct dirent) <= dir_sz; off += sizeof(struct dirent)){
    struct dirent de;
    struct dinode ent;
    char name[DIRSIZ + 1];
    if(inode_read_range(&dir, off, &de, sizeof(de)) != 0){
      err = EIO;
      goto out;
    }
    if(de.inum == 0)
      continue;
    memset(name, 0, sizeof(name));
    memcpy(name, de.name, DIRSIZ);
    if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;
    if(seen++ != index)
      continue;

    strncpy(name_out, name, name_out_len - 1);
    name_out[name_out_len - 1] = 0;
    if(read_inode(de.inum, &ent) != 0){
      err = EIO;
      goto out;
    }
    if(type_out)
      *type_out = ent.type;
    if(size_out)
      *size_out = inode_get_size(&ent);
    rc = 0;
    goto out;
  }

out:
  vfs_unlock();
  if(rc != 0)
    task_ctx_set_errno(err);
  return rc;
}

int xv6fs_list_fd(int fd, int index, char *name_out, int name_out_len, uint16 *type_out, uint32 *size_out)
{
  int real_fd = stdio_map_fd(fd);
  struct dinode dir;
  uint32 off;
  uint32 dir_sz;
  int seen = 0;
  int rc = -1;
  int err = EBADF;

  task_ctx_clear_errno();
  if(index < 0 || name_out == 0 || name_out_len <= 1){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(!g_ready){
    task_ctx_set_errno(ENODEV);
    return -1;
  }
  if(real_fd < 0 || real_fd >= XV6_MAX_FD){
    task_ctx_set_errno(EBADF);
    return -1;
  }

  vfs_lock();
  if(!g_fds[real_fd].used){
    err = EBADF;
    goto out;
  }
  if(g_fds[real_fd].kind != VFD_FILE){
    err = EBADF;
    goto out;
  }
  if(read_inode(g_fds[real_fd].inum, &dir) != 0){
    err = EIO;
    goto out;
  }
  if(dir.type != T_DIR){
    err = ENOTDIR;
    goto out;
  }
  dir_sz = inode_get_size(&dir);

  for(off = 0; off + sizeof(struct dirent) <= dir_sz; off += sizeof(struct dirent)){
    struct dirent de;
    struct dinode ent;
    char name[DIRSIZ + 1];
    if(inode_read_range(&dir, off, &de, sizeof(de)) != 0){
      err = EIO;
      goto out;
    }
    if(de.inum == 0)
      continue;
    memset(name, 0, sizeof(name));
    memcpy(name, de.name, DIRSIZ);
    if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;
    if(seen++ != index)
      continue;

    strncpy(name_out, name, name_out_len - 1);
    name_out[name_out_len - 1] = 0;
    if(read_inode(de.inum, &ent) != 0){
      err = EIO;
      goto out;
    }
    if(type_out)
      *type_out = ent.type;
    if(size_out)
      *size_out = inode_get_size(&ent);
    rc = 0;
    goto out;
  }

  err = ENOENT;

out:
  vfs_unlock();
  if(rc != 0)
    task_ctx_set_errno(err);
  return rc;
}

int xv6fs_read_file_alloc_path(const char *path, void **out_data, uint32 *out_size)
{
  char abs_path[VFS_PATH_MAX];
  uint32 inum;
  struct dinode ip;
  void *buf;
  int rc = -1;
  int err = EIO;

  task_ctx_clear_errno();
  if(path == 0 || out_data == 0 || out_size == 0){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(!g_ready){
    task_ctx_set_errno(ENODEV);
    return -1;
  }
  if(path_resolve(path, abs_path, sizeof(abs_path)) != 0){
    task_ctx_set_errno(ENOENT);
    return -1;
  }
  if(is_dev_node(abs_path)){
    rc = dev_read_alloc(abs_path, out_data, out_size);
    if(rc != 0)
      task_ctx_set_errno(EIO);
    return rc;
  }
  vfs_lock();
  if(path_lookup(abs_path, &inum, &ip) != 0){
    err = ENOENT;
    goto out_unlock;
  }
  if(ip.type != T_FILE){
    err = EISDIR;
    goto out_unlock;
  }

  buf = malloc(inode_get_size(&ip) ? inode_get_size(&ip) : 1);
  if(buf == 0){
    err = ENOMEM;
    rc = -1;
    goto out_unlock;
  }
  if(inode_get_size(&ip) > 0 && inode_read_range(&ip, 0, buf, inode_get_size(&ip)) != 0){
    free(buf);
    err = EIO;
    rc = -1;
    goto out_unlock;
  }
  *out_data = buf;
  *out_size = inode_get_size(&ip);
  rc = 0;

out_unlock:
  vfs_unlock();
  if(rc != 0)
    task_ctx_set_errno(err);
  return rc;
}

int xv6fs_mkdir_path(const char *path)
{
  char abs_path[VFS_PATH_MAX];
  uint32 pinum;
  uint32 inum;
  struct dinode pip;
  struct dinode exist;
  struct dinode newdir;
  char name[DIRSIZ + 1];
  struct dirent de;
  int rc = -1;
  int created = 0;
  int linked = 0;
  int err = EIO;

  task_ctx_clear_errno();
  if(path == 0 || !g_ready){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(path_resolve(path, abs_path, sizeof(abs_path)) != 0){
    task_ctx_set_errno(ENOENT);
    return -1;
  }
  vfs_lock();
  if(path_lookup(abs_path, &inum, &exist) == 0){
    err = EEXIST;
    goto out_err;
  }
  if(path_parent(abs_path, &pinum, name) != 0){
    err = ENOENT;
    goto out_err;
  }
  if(read_inode(pinum, &pip) != 0){
    err = EIO;
    goto out_err;
  }
  if(pip.type != T_DIR){
    err = ENOTDIR;
    goto out_err;
  }
  if(alloc_inode(T_DIR, &inum) != 0){
    err = ENOSPC;
    goto out_err;
  }
  created = 1;
  if(read_inode(inum, &newdir) != 0){
    err = EIO;
    goto out_err;
  }

  memset(&de, 0, sizeof(de));
  de.inum = inum;
  memcpy(de.name, ".", 1);
  if(inode_write_range(&newdir, 0, &de, sizeof(de)) != 0){
    err = EIO;
    goto out_err;
  }
  memset(&de, 0, sizeof(de));
  de.inum = pinum;
  memcpy(de.name, "..", 2);
  if(inode_write_range(&newdir, sizeof(de), &de, sizeof(de)) != 0){
    err = EIO;
    goto out_err;
  }
  if(write_inode(inum, &newdir) != 0){
    err = EIO;
    goto out_err;
  }
  rc = dir_add_entry(pinum, name, inum);
  if(rc == 0){
    linked = 1;
  } else {
    err = ENOSPC;
  }

out_err:
  if(rc != 0 && created && !linked)
    inode_reclaim_orphan_locked(inum);
  if(rc != 0)
    task_ctx_set_errno(err);
  vfs_unlock();
  return rc;
}

int xv6fs_mkfifo_path(const char *path)
{
  char abs_path[VFS_PATH_MAX];
  char name[DIRSIZ + 1];
  uint32 pinum = 0;
  uint32 inum = 0;
  int lookup_rc;
  int created = 0;
  int linked = 0;
  int err = EIO;

  task_ctx_clear_errno();
  if(path == 0 || !g_ready){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(path_resolve(path, abs_path, sizeof(abs_path)) != 0){
    task_ctx_set_errno(ENOENT);
    return -1;
  }
  if(is_dev_node(abs_path)){
    task_ctx_set_errno(EPERM);
    return -1;
  }

  vfs_lock();
  lookup_rc = path_lookup(abs_path, 0, 0);
  if(lookup_rc == 0){
    err = EEXIST;
    goto out;
  }
  if(lookup_rc != 1){
    err = EIO;
    goto out;
  }
  if(path_parent(abs_path, &pinum, name) != 0){
    err = ENOENT;
    goto out;
  }
  if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0){
    err = EINVAL;
    goto out;
  }

  if(alloc_inode(T_FIFO, &inum) != 0){
    err = ENOSPC;
    goto out;
  }
  created = 1;
  if(dir_add_entry(pinum, name, inum) != 0){
    err = ENOSPC;
    goto out;
  }
  linked = 1;
  vfs_unlock();
  return 0;

out:
  if(created && !linked)
    inode_reclaim_orphan_locked(inum);
  task_ctx_set_errno(err);
  vfs_unlock();
  return -1;
}

int xv6fs_write_file_path(const char *path, const void *data, uint32 size)
{
  char abs_path[VFS_PATH_MAX];
  uint32 pinum, inum;
  char name[DIRSIZ + 1];
  struct dinode ip;
  int rc = -1;
  int lookup_rc;
  int created = 0;
  int linked = 0;
  int err = EIO;

  task_ctx_clear_errno();
  if(path == 0 || data == 0){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(!g_ready){
    task_ctx_set_errno(ENODEV);
    return -1;
  }
  if(path_resolve(path, abs_path, sizeof(abs_path)) != 0){
    task_ctx_set_errno(ENOENT);
    return -1;
  }
  if(is_dev_node(abs_path)){
    rc = dev_write(abs_path, data, size);
    if(rc < 0)
      task_ctx_set_errno(EIO);
    return rc;
  }
  vfs_lock();
  if(path_parent(abs_path, &pinum, name) != 0){
    err = ENOENT;
    goto out;
  }

  lookup_rc = dir_lookup_inum(pinum, name, &inum, &ip);
  if(lookup_rc == 1){
    if(alloc_inode(T_FILE, &inum) != 0){
      err = ENOSPC;
      goto out;
    }
    created = 1;
    if(read_inode(inum, &ip) != 0){
      err = EIO;
      goto out;
    }
    if(dir_add_entry(pinum, name, inum) != 0){
      err = ENOSPC;
      goto out;
    }
    linked = 1;
  } else if(lookup_rc != 0){
    err = EIO;
    goto out;
  } else if(ip.type != T_FILE){
    err = EISDIR;
    goto out;
  }

  if(inode_truncate(inum, &ip) != 0){
    err = EIO;
    goto out;
  }
  if(read_inode(inum, &ip) != 0){
    err = EIO;
    goto out;
  }
  if(size > 0 && inode_write_range(&ip, 0, data, size) != 0){
    err = EIO;
    goto out;
  }
  rc = write_inode(inum, &ip);
  if(rc != 0)
    err = EIO;

out:
  if(rc != 0 && created && !linked)
    inode_reclaim_orphan_locked(inum);
  vfs_unlock();
  if(rc != 0)
    task_ctx_set_errno(err);
  return rc;
}

int xv6fs_unlink_path(const char *path)
{
  char abs_path[VFS_PATH_MAX];
  uint32 pinum, inum, off;
  char name[DIRSIZ + 1];
  struct dirent de;
  struct dinode ip;
  int err = EIO;
  int removed_entry = 0;

  task_ctx_clear_errno();
  if(path == 0 || !g_ready){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(path_resolve(path, abs_path, sizeof(abs_path)) != 0){
    task_ctx_set_errno(ENOENT);
    return -1;
  }
  vfs_lock();
  if(strcmp(abs_path, "/") == 0){
    err = EISDIR;
    goto out_fail;
  }
  if(is_dev_node(abs_path)){
    err = EPERM;
    goto out_fail;
  }
  if(path_parent(abs_path, &pinum, name) != 0){
    err = ENOENT;
    goto out_fail;
  }
  if(dir_find_entry_offset(pinum, name, &off, &de) != 0){
    err = ENOENT;
    goto out_fail;
  }

  inum = de.inum;
  if(read_inode(inum, &ip) != 0){
    err = EIO;
    goto out_fail;
  }
  if(ip.type == T_DIR){
    err = EISDIR;
    goto out_fail;
  }
  if(dir_clear_entry_at(pinum, off) != 0){
    err = EIO;
    goto out_fail;
  }
  removed_entry = 1;
  if(inode_drop_link_locked(inum, &ip) != 0){
    err = EIO;
    goto out_rollback;
  }
  vfs_unlock();
  return 0;

out_rollback:
  if(removed_entry)
    (void)dir_replace_entry_at(pinum, off, &de);
  if(removed_entry)
    err = EIO;
out_fail:
  task_ctx_set_errno(err);
  vfs_unlock();
  return -1;
}

int xv6fs_rmdir_path(const char *path)
{
  char abs_path[VFS_PATH_MAX];
  uint32 pinum, inum, off;
  char name[DIRSIZ + 1];
  struct dirent de;
  struct dinode ip;
  int rc = -1;
  int empty_rc;
  int err = EIO;
  int removed_entry = 0;

  task_ctx_clear_errno();
  if(path == 0 || !g_ready){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(path_resolve(path, abs_path, sizeof(abs_path)) != 0){
    task_ctx_set_errno(ENOENT);
    return -1;
  }
  vfs_lock();
  if(strcmp(abs_path, "/") == 0){
    err = EBUSY;
    goto out_err;
  }
  if(is_dev_node(abs_path)){
    err = EPERM;
    goto out_err;
  }
  if(path_parent(abs_path, &pinum, name) != 0){
    err = ENOENT;
    goto out_err;
  }
  if(dir_find_entry_offset(pinum, name, &off, &de) != 0){
    err = ENOENT;
    goto out_err;
  }

  inum = de.inum;
  if(read_inode(inum, &ip) != 0){
    err = EIO;
    goto out_err;
  }
  if(ip.type != T_DIR){
    err = ENOTDIR;
    goto out_err;
  }
  empty_rc = dir_is_empty(inum);
  if(empty_rc < 0){
    err = EIO;
    goto out_err;
  }
  if(empty_rc == 0){
    err = ENOTEMPTY;
    goto out_err;
  }
  if(dir_clear_entry_at(pinum, off) != 0){
    err = EIO;
    goto out_err;
  }
  removed_entry = 1;
  if(inode_truncate(inum, &ip) != 0){
    err = EIO;
    goto out_rollback;
  }
  memset(&ip, 0, sizeof(ip));
  rc = write_inode(inum, &ip);
  if(rc != 0){
    err = EIO;
    goto out_rollback;
  }
  vfs_unlock();
  return 0;

out_rollback:
  if(removed_entry)
    (void)dir_replace_entry_at(pinum, off, &de);
out_err:
  task_ctx_set_errno(err);
  vfs_unlock();
  return -1;
}

int xv6fs_rename_path(const char *oldpath, const char *newpath)
{
  char old_abs[VFS_PATH_MAX];
  char new_abs[VFS_PATH_MAX];
  char old_name[DIRSIZ + 1];
  char new_name[DIRSIZ + 1];
  struct dirent old_de;
  struct dirent new_de;
  struct dinode old_ip;
  struct dinode new_ip;
  uint32 old_parent;
  uint32 new_parent;
  uint32 old_off = 0;
  uint32 new_off = 0;
  uint32 new_inum = 0;
  int new_exists = 0;
  int replace_existing = 0;
  int dest_touched = 0;
  int err = EIO;
  int lookup_rc;
  int moved_cross_parent;
  size_t old_len;

  task_ctx_clear_errno();
  if(oldpath == 0 || newpath == 0 || !g_ready){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(path_resolve(oldpath, old_abs, sizeof(old_abs)) != 0 || path_resolve(newpath, new_abs, sizeof(new_abs)) != 0){
    task_ctx_set_errno(ENOENT);
    return -1;
  }
  if(strcmp(old_abs, new_abs) == 0)
    return 0;

  vfs_lock();
  moved_cross_parent = 0;
  if(strcmp(old_abs, "/") == 0 || strcmp(new_abs, "/") == 0){
    err = EBUSY;
    goto out_fail;
  }
  if(is_dev_node(old_abs) || is_dev_node(new_abs)){
    err = EPERM;
    goto out_fail;
  }
  if(path_parent(old_abs, &old_parent, old_name) != 0){
    err = ENOENT;
    goto out_fail;
  }
  if(path_parent(new_abs, &new_parent, new_name) != 0){
    err = ENOENT;
    goto out_fail;
  }
  if(strcmp(new_name, ".") == 0 || strcmp(new_name, "..") == 0){
    err = EINVAL;
    goto out_fail;
  }
  if(dir_find_entry_offset(old_parent, old_name, &old_off, &old_de) != 0){
    err = ENOENT;
    goto out_fail;
  }
  if(read_inode(old_de.inum, &old_ip) != 0){
    err = EIO;
    goto out_fail;
  }
  lookup_rc = dir_find_entry_offset(new_parent, new_name, &new_off, &new_de);
  if(lookup_rc == 0){
    new_exists = 1;
    new_inum = new_de.inum;
    if(new_inum == old_de.inum){
      if(old_parent == new_parent && old_off == new_off){
        vfs_unlock();
        return 0;
      }
      err = EINVAL;
      goto out_fail;
    }
    if(read_inode(new_inum, &new_ip) != 0){
      err = EIO;
      goto out_fail;
    }
    if(old_ip.type == T_DIR && new_ip.type != T_DIR){
      err = ENOTDIR;
      goto out_fail;
    }
    if(old_ip.type != T_DIR && new_ip.type == T_DIR){
      err = EISDIR;
      goto out_fail;
    }
    if(new_ip.type == T_DIR){
      int empty_rc = dir_is_empty(new_inum);
      if(empty_rc < 0){
        err = EIO;
        goto out_fail;
      }
      if(empty_rc == 0){
        err = ENOTEMPTY;
        goto out_fail;
      }
    }
  } else if(lookup_rc != 1){
    err = EIO;
    goto out_fail;
  }

  if(old_ip.type == T_DIR){
    old_len = strlen(old_abs);
    if(strncmp(new_abs, old_abs, old_len) == 0 && (new_abs[old_len] == 0 || new_abs[old_len] == '/')){
      err = EINVAL;
      goto out_fail;
    }
  }

  moved_cross_parent = (old_parent != new_parent);

  if(!new_exists && old_parent == new_parent){
    struct dirent renamed = old_de;
    memset(renamed.name, 0, sizeof(renamed.name));
    memcpy(renamed.name, new_name, strlen(new_name));
    if(dir_replace_entry_at(old_parent, old_off, &renamed) != 0){
      err = EIO;
      goto out_fail;
    }
    vfs_unlock();
    return 0;
  }

  if(new_exists){
    struct dirent replacement = new_de;
    replacement.inum = old_de.inum;
    memset(replacement.name, 0, sizeof(replacement.name));
    memcpy(replacement.name, new_name, strlen(new_name));
    if(dir_replace_entry_at(new_parent, new_off, &replacement) != 0){
      err = EIO;
      goto out_fail;
    }
    dest_touched = 1;
    replace_existing = 1;
  } else {
    if(dir_add_entry(new_parent, new_name, old_de.inum) != 0){
      err = ENOSPC;
      goto out_fail;
    }
    dest_touched = 1;
  }

  if(old_ip.type == T_DIR && moved_cross_parent && dir_set_dotdot(old_de.inum, new_parent) != 0){
    err = EIO;
    goto out_rollback;
  }
  if(dir_clear_entry_at(old_parent, old_off) != 0){
    err = EIO;
    goto out_rollback;
  }
  if(replace_existing && inode_drop_link_locked(new_inum, &new_ip) != 0){
    err = EIO;
    /*
     * Source entry is already cleared at this point. Restore both directory
     * entries and ".." for cross-parent directory moves to avoid partial
     * rename state when target inode update fails.
     */
    if(old_ip.type == T_DIR && moved_cross_parent)
      (void)dir_set_dotdot(old_de.inum, old_parent);
    (void)dir_replace_entry_at(new_parent, new_off, &new_de);
    (void)dir_replace_entry_at(old_parent, old_off, &old_de);
    goto out_fail;
  }
  vfs_unlock();
  return 0;

out_rollback:
  if(old_ip.type == T_DIR && moved_cross_parent)
    (void)dir_set_dotdot(old_de.inum, old_parent);
  if(dest_touched){
    if(new_exists){
      (void)dir_replace_entry_at(new_parent, new_off, &new_de);
    } else {
      struct dirent cur_de;
      if(dir_find_entry_offset(new_parent, new_name, &new_off, &cur_de) == 0)
        (void)dir_clear_entry_at(new_parent, new_off);
    }
  }
out_fail:
  task_ctx_set_errno(err);

  vfs_unlock();
  return -1;
}

int xv6fs_link_path(const char *oldpath, const char *newpath)
{
  char old_abs[VFS_PATH_MAX];
  char new_abs[VFS_PATH_MAX];
  char old_lookup[VFS_PATH_MAX];
  char new_name[DIRSIZ + 1];
  uint32 old_inum = 0;
  uint32 new_parent = 0;
  uint32 new_off = 0;
  struct dinode old_ip;
  struct dirent new_de;
  int lookup_rc;
  int added_entry = 0;
  int err = EIO;

  task_ctx_clear_errno();
  if(oldpath == 0 || newpath == 0 || !g_ready){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(path_resolve(oldpath, old_abs, sizeof(old_abs)) != 0 || path_resolve(newpath, new_abs, sizeof(new_abs)) != 0){
    task_ctx_set_errno(ENOENT);
    return -1;
  }
  if(strcmp(new_abs, "/") == 0){
    task_ctx_set_errno(EEXIST);
    return -1;
  }
  if(is_dev_node(old_abs) || is_dev_node(new_abs)){
    task_ctx_set_errno(EPERM);
    return -1;
  }

  vfs_lock();
  if(path_lookup_follow_locked(old_abs, 1, old_lookup, sizeof(old_lookup), &old_inum, &old_ip) != 0){
    err = (xv6_last_errno() == ELOOP) ? ELOOP : ENOENT;
    goto out_fail;
  }
  if(old_ip.type == T_DIR){
    err = EPERM;
    goto out_fail;
  }
  if(path_parent(new_abs, &new_parent, new_name) != 0){
    err = ENOENT;
    goto out_fail;
  }
  if(strcmp(new_name, ".") == 0 || strcmp(new_name, "..") == 0){
    err = EINVAL;
    goto out_fail;
  }

  lookup_rc = dir_find_entry_offset(new_parent, new_name, &new_off, &new_de);
  if(lookup_rc == 0){
    err = EEXIST;
    goto out_fail;
  }
  if(lookup_rc != 1){
    err = EIO;
    goto out_fail;
  }

  if(dir_add_entry(new_parent, new_name, old_inum) != 0){
    err = ENOSPC;
    goto out_fail;
  }
  added_entry = 1;

  if(inode_add_link_locked(old_inum, &old_ip) != 0){
    err = EMLINK;
    goto out_rollback;
  }

  vfs_unlock();
  return 0;

out_rollback:
  if(added_entry && dir_find_entry_offset(new_parent, new_name, &new_off, &new_de) == 0)
    (void)dir_clear_entry_at(new_parent, new_off);
out_fail:
  task_ctx_set_errno(err);
  vfs_unlock();
  return -1;
}

int xv6fs_symlink_path(const char *target, const char *linkpath)
{
  char target_buf[XV6_SYMLINK_TARGET_MAX];
  char abs_path[VFS_PATH_MAX];
  char name[DIRSIZ + 1];
  uint32 pinum = 0;
  uint32 inum = 0;
  uint32 target_len;
  struct dinode ip;
  int lookup_rc;
  int created = 0;
  int linked = 0;
  int err = EIO;

  task_ctx_clear_errno();
  if(target == 0 || linkpath == 0 || !g_ready){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(copy_guest_cstr(target, target_buf, sizeof(target_buf)) != 0){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  target_len = (uint32)strlen(target_buf);
  if(path_resolve(linkpath, abs_path, sizeof(abs_path)) != 0){
    task_ctx_set_errno(ENOENT);
    return -1;
  }
  if(is_dev_node(abs_path)){
    task_ctx_set_errno(EPERM);
    return -1;
  }

  vfs_lock();
  lookup_rc = path_lookup(abs_path, 0, 0);
  if(lookup_rc == 0){
    err = EEXIST;
    goto out;
  }
  if(lookup_rc != 1){
    err = EIO;
    goto out;
  }
  if(path_parent(abs_path, &pinum, name) != 0){
    err = ENOENT;
    goto out;
  }
  if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0){
    err = EINVAL;
    goto out;
  }

  if(alloc_inode(T_SYMLINK, &inum) != 0){
    err = ENOSPC;
    goto out;
  }
  created = 1;
  if(read_inode(inum, &ip) != 0){
    err = EIO;
    goto out;
  }
  if(target_len > 0 && inode_write_range(&ip, 0, target_buf, target_len) != 0){
    err = ENOSPC;
    goto out;
  }
  if(write_inode(inum, &ip) != 0){
    err = EIO;
    goto out;
  }
  if(dir_add_entry(pinum, name, inum) != 0){
    err = ENOSPC;
    goto out;
  }
  linked = 1;
  vfs_unlock();
  return 0;

out:
  if(created && !linked)
    inode_reclaim_orphan_locked(inum);
  task_ctx_set_errno(err);
  vfs_unlock();
  return -1;
}

int xv6fs_readlink_path(const char *path, char *buf, uint32 bufsz)
{
  char abs_path[VFS_PATH_MAX];
  uint32 inum = 0;
  uint32 size = 0;
  uint32 copy_n = 0;
  struct dinode ip;
  int err = EIO;

  task_ctx_clear_errno();
  if(path == 0 || buf == 0 || bufsz == 0 || !g_ready){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(path_resolve(path, abs_path, sizeof(abs_path)) != 0){
    task_ctx_set_errno(ENOENT);
    return -1;
  }
  if(is_dev_node(abs_path)){
    task_ctx_set_errno(EINVAL);
    return -1;
  }

  vfs_lock();
  if(path_lookup(abs_path, &inum, &ip) != 0){
    err = ENOENT;
    goto out_fail;
  }
  if(ip.type != T_SYMLINK){
    err = EINVAL;
    goto out_fail;
  }
  size = inode_get_size(&ip);
  copy_n = (size < bufsz) ? size : bufsz;
  if(copy_n > 0 && inode_read_range(&ip, 0, buf, copy_n) != 0){
    err = EIO;
    goto out_fail;
  }

  vfs_unlock();
  return (int)copy_n;

out_fail:
  task_ctx_set_errno(err);
  vfs_unlock();
  return -1;
}

static int vfs_alloc_fd(void)
{
  int i;
  if(g_fds == 0)
    return -1;
  for(i = 3; i < XV6_MAX_FD; i++){
    if(!g_fds[i].used)
      return i;
  }
  return -1;
}

static int fd_group_in_use_locked(int group_id)
{
  int i;
  if(group_id <= 0)
    return 0;
  for(i = 3; i < XV6_MAX_FD; i++){
    if(g_fds[i].used && g_fds[i].group_id == group_id)
      return 1;
  }
  return 0;
}

static int fd_group_alloc_locked(void)
{
  int attempts = XV6_MAX_FD + 1;

  if(g_next_fd_group < 1)
    g_next_fd_group = 1;
  while(attempts-- > 0){
    int id = g_next_fd_group++;
    if(g_next_fd_group < 1)
      g_next_fd_group = 1;
    if(!fd_group_in_use_locked(id))
      return id;
  }
  return -1;
}

static uint32 fd_group_get_off_locked(int fd)
{
  int i;
  int gid = g_fds[fd].group_id;

  if(gid <= 0)
    return g_fds[fd].off;
  for(i = 3; i < XV6_MAX_FD; i++){
    if(g_fds[i].used && g_fds[i].group_id == gid)
      return g_fds[i].off;
  }
  return g_fds[fd].off;
}

static void fd_group_set_off_locked(int fd, uint32 off)
{
  int i;
  int gid = g_fds[fd].group_id;

  if(gid <= 0){
    g_fds[fd].off = off;
    return;
  }
  for(i = 3; i < XV6_MAX_FD; i++){
    if(g_fds[i].used && g_fds[i].group_id == gid)
      g_fds[i].off = off;
  }
}

static int vfs_create_regular_file(const char *path, uint32 *out_inum)
{
  uint32 pinum, inum;
  char name[DIRSIZ + 1];
  struct dinode ip;
  int lookup_rc;

  if(path_parent(path, &pinum, name) != 0)
    return -1;
  lookup_rc = dir_lookup_inum(pinum, name, &inum, &ip);
  if(lookup_rc == 0){
    if(ip.type != T_FILE)
      return -1;
    *out_inum = inum;
    return 0;
  }
  if(lookup_rc != 1)
    return -1;

  if(alloc_inode(T_FILE, &inum) != 0)
    return -1;
  if(dir_add_entry(pinum, name, inum) != 0){
    inode_reclaim_orphan_locked(inum);
    return -1;
  }
  *out_inum = inum;
  return 0;
}

void xv6_vfs_reset(void)
{
  xv6_task_ctx_t *ctx;
  int i;
  if(g_vfs_lock == 0)
    g_vfs_lock = xSemaphoreCreateMutex();
  vfs_lock();
  if(fd_table_ensure_locked() != 0){
    g_ready = 0;
    vfs_unlock();
    ESP_LOGE(TAG, "fd table allocation failed");
    return;
  }
  memset(g_fds, 0, (size_t)g_fds_cap * sizeof(*g_fds));
  for(i = 0; i < XV6_MAX_PTY; i++)
    pty_slot_reset(&g_ptys[i]);
  for(i = 0; i < XV6_MAX_PIPE; i++)
    pipe_slot_reset(&g_pipes[i]);
  memset(g_fifo_links, 0, sizeof(g_fifo_links));
  g_next_fd_group = 1;

  g_fds[0].used = 1;
  g_fds[0].kind = VFD_DEV;
  g_fds[0].flags = XV6_O_RDONLY;
  g_fds[0].group_id = 0;
  g_fds[0].owner = 0;
  copy_cstr(g_fds[0].path, sizeof(g_fds[0].path), "/dev/stdin");

  g_fds[1].used = 1;
  g_fds[1].kind = VFD_DEV;
  g_fds[1].flags = XV6_O_WRONLY;
  g_fds[1].group_id = 0;
  g_fds[1].owner = 0;
  copy_cstr(g_fds[1].path, sizeof(g_fds[1].path), "/dev/stdout");

  g_fds[2].used = 1;
  g_fds[2].kind = VFD_DEV;
  g_fds[2].flags = XV6_O_WRONLY;
  g_fds[2].group_id = 0;
  g_fds[2].owner = 0;
  copy_cstr(g_fds[2].path, sizeof(g_fds[2].path), "/dev/stderr");
  vfs_unlock();
  task_ctx_reset_all();
  ctx = task_ctx_get(1);
  if(ctx == 0)
    ESP_LOGW(TAG, "task ctx allocation failed on reset");
}

int xv6_open(const char *path, int flags)
{
  int fd;
  int err = EIO;
  int rc;
  char abs_path[VFS_PATH_MAX];
  char resolved_path[VFS_PATH_MAX];
  char canon[VFS_PATH_MAX];
  int pty_id = -1;
  uint32 inum;
  struct dinode ip;

  task_ctx_clear_errno();
  if(!g_ready){
    task_ctx_set_errno(ENODEV);
    return -1;
  }
  if(path == 0 || path[0] == 0){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(path_resolve(path, abs_path, sizeof(abs_path)) != 0){
    task_ctx_set_errno(ENOENT);
    return -1;
  }
  vfs_lock();
  fd = vfs_alloc_fd();
  if(fd < 0){
    err = EMFILE;
    goto fail_unlock;
  }

  memset(&g_fds[fd], 0, sizeof(g_fds[fd]));
  g_fds[fd].used = 1;
  g_fds[fd].flags = flags;
  g_fds[fd].group_id = 0;
  g_fds[fd].owner = xTaskGetCurrentTaskHandle();

  rc = path_lookup(abs_path, &inum, &ip);
  if(rc == 0 && ip.type == T_SYMLINK){
    int allow_missing_final = ((flags & XV6_O_CREAT) != 0);
    if(path_resolve_final_symlink_locked_impl(abs_path, resolved_path, sizeof(resolved_path), allow_missing_final) !=
       0){
      err = (xv6_last_errno() == ELOOP) ? ELOOP : ENOENT;
      goto fail;
    }
    copy_cstr(abs_path, sizeof(abs_path), resolved_path);
  }

  if(is_dev_node(abs_path)){
    if(dev_canonical_path(abs_path, canon, sizeof(canon)) != 0){
      err = ENOENT;
      goto fail;
    }
    g_fds[fd].kind = VFD_DEV;
    copy_cstr(g_fds[fd].path, sizeof(g_fds[fd].path), canon);
    if(strcmp(canon, "/dev/ptmx") == 0){
      pty_id = pty_alloc_id();
      if(pty_id < 0){
        err = ENOSPC;
        goto fail;
      }
      g_ptys[pty_id].master_open = 1;
      g_fds[fd].dev_role = DEV_ROLE_PTY_MASTER;
      g_fds[fd].dev_id = pty_id;
    } else if(parse_pts_id(canon, &pty_id) == 0){
      if(!g_ptys[pty_id].alloc){
        err = ENOENT;
        goto fail;
      }
      if(g_ptys[pty_id].slave_open){
        err = EBUSY;
        goto fail;
      }
      g_ptys[pty_id].slave_open = 1;
      g_fds[fd].dev_role = DEV_ROLE_PTY_SLAVE;
      g_fds[fd].dev_id = pty_id;
    }
    vfs_unlock();
    return fd;
  }

  if(path_lookup(abs_path, &inum, &ip) != 0){
    if((flags & XV6_O_CREAT) == 0){
      err = ENOENT;
      goto fail;
    }
    if(vfs_create_regular_file(abs_path, &inum) != 0){
      err = ENOSPC;
      goto fail;
    }
    if(read_inode(inum, &ip) != 0 || ip.type != T_FILE){
      err = EIO;
      goto fail;
    }
  } else if(ip.type == T_DIR){
    if((flags & XV6_O_ACCMODE) != XV6_O_RDONLY || (flags & (XV6_O_CREAT | XV6_O_TRUNC | XV6_O_APPEND)) != 0){
      err = EISDIR;
      goto fail;
    }
  } else if(ip.type == T_FIFO){
    int pipe_id;
    int add_readers = 0;
    int add_writers = 0;

    if(pipe_endpoints_from_flags(flags, &add_readers, &add_writers) != 0){
      err = EINVAL;
      goto fail;
    }
    pipe_id = fifo_bind_or_get_pipe_locked(inum);
    if(pipe_id < 0){
      err = ENFILE;
      goto fail;
    }
    g_fds[fd].kind = VFD_PIPE;
    g_fds[fd].dev_id = pipe_id;
    g_fds[fd].inum = inum;
    copy_cstr(g_fds[fd].path, sizeof(g_fds[fd].path), abs_path);
    g_pipes[pipe_id].readers += add_readers;
    g_pipes[pipe_id].writers += add_writers;
    vfs_unlock();
    return fd;
  } else if(ip.type != T_FILE){
    err = ENOENT;
    goto fail;
  }

  g_fds[fd].kind = VFD_FILE;
  g_fds[fd].inum = inum;
  g_fds[fd].group_id = fd_group_alloc_locked();
  if(g_fds[fd].group_id < 1){
    err = ENFILE;
    goto fail;
  }
  g_fds[fd].off = 0;
  if(flags & XV6_O_TRUNC){
    if(ip.type != T_FILE){
      err = EISDIR;
      goto fail;
    }
    if(inode_truncate(inum, &ip) != 0){
      err = EIO;
      goto fail;
    }
  } else if((flags & XV6_O_APPEND) && ip.type == T_FILE){
    g_fds[fd].off = inode_get_size(&ip);
  }
  copy_cstr(g_fds[fd].path, sizeof(g_fds[fd].path), abs_path);
  fd_group_set_off_locked(fd, g_fds[fd].off);
  vfs_unlock();
  return fd;

fail:
  memset(&g_fds[fd], 0, sizeof(g_fds[fd]));
fail_unlock:
  task_ctx_set_errno(err);
  vfs_unlock();
  return -1;
}

int xv6_lseek(int fd, int offset, int whence)
{
  int real_fd = stdio_map_fd(fd);
  struct dinode ip;
  uint32 cur_off;
  long long base_off = 0;
  long long wanted_off = 0;
  int err = EINVAL;

  task_ctx_clear_errno();
  if(real_fd < 0 || real_fd >= XV6_MAX_FD){
    task_ctx_set_errno(EBADF);
    return -1;
  }

  vfs_lock();
  if(!g_fds[real_fd].used){
    err = EBADF;
    goto fail;
  }
  if(g_fds[real_fd].kind != VFD_FILE){
    err = ESPIPE;
    goto fail;
  }
  if(read_inode(g_fds[real_fd].inum, &ip) != 0){
    err = EIO;
    goto fail;
  }
  if(ip.type != T_FILE && ip.type != T_DIR){
    err = ESPIPE;
    goto fail;
  }
  cur_off = fd_group_get_off_locked(real_fd);

  if(whence == 0)
    base_off = 0;
  else if(whence == 1)
    base_off = (long long)cur_off;
  else if(whence == 2)
    base_off = (long long)inode_get_size(&ip);
  else {
    err = EINVAL;
    goto fail;
  }
  wanted_off = base_off + (long long)offset;

  if(wanted_off < 0){
    err = EINVAL;
    goto fail;
  }
  if(wanted_off > 0x7fffffffLL){
    err = EINVAL;
    goto fail;
  }

  fd_group_set_off_locked(real_fd, (uint32)wanted_off);
  vfs_unlock();
  return (int)wanted_off;

fail:
  task_ctx_set_errno(err);
  vfs_unlock();
  return -1;
}

int xv6_set_status_flags(int fd, int status_flags)
{
  int real_fd = stdio_map_fd(fd);
  int gid;
  int i;

  task_ctx_clear_errno();
  if(real_fd < 0 || real_fd >= XV6_MAX_FD){
    task_ctx_set_errno(EBADF);
    return -1;
  }

  vfs_lock();
  if(!g_fds[real_fd].used){
    task_ctx_set_errno(EBADF);
    vfs_unlock();
    return -1;
  }
  gid = g_fds[real_fd].group_id;
  if(gid <= 0){
    g_fds[real_fd].flags = (g_fds[real_fd].flags & ~XV6_O_APPEND) | (status_flags & XV6_O_APPEND);
  } else {
    for(i = 3; i < XV6_MAX_FD; i++){
      if(g_fds[i].used && g_fds[i].group_id == gid)
        g_fds[i].flags = (g_fds[i].flags & ~XV6_O_APPEND) | (status_flags & XV6_O_APPEND);
    }
  }
  vfs_unlock();
  return 0;
}

int xv6_ftruncate(int fd, long long length)
{
  int real_fd = stdio_map_fd(fd);
  uint32 inum;
  uint32 new_size;
  struct dinode ip;
  int err = EINVAL;

  task_ctx_clear_errno();
  if(length < 0){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if((uint64)length > inode_max_file_size()){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(real_fd < 0 || real_fd >= XV6_MAX_FD){
    task_ctx_set_errno(EBADF);
    return -1;
  }
  new_size = (uint32)length;

  vfs_lock();
  if(!g_fds[real_fd].used){
    err = EBADF;
    goto out_fail;
  }
  if(g_fds[real_fd].kind != VFD_FILE){
    err = EBADF;
    goto out_fail;
  }

  inum = g_fds[real_fd].inum;
  if(read_inode(inum, &ip) != 0){
    err = EIO;
    goto out_fail;
  }
  if(ip.type != T_FILE){
    err = EISDIR;
    goto out_fail;
  }
  if(inode_resize(inum, &ip, new_size) != 0){
    err = EIO;
    goto out_fail;
  }
  if(fd_group_get_off_locked(real_fd) > new_size)
    fd_group_set_off_locked(real_fd, new_size);
  vfs_unlock();
  return 0;

out_fail:
  task_ctx_set_errno(err);
  vfs_unlock();
  return -1;
}

int xv6_truncate_path(const char *path, long long length)
{
  char abs_path[VFS_PATH_MAX];
  char resolved_path[VFS_PATH_MAX];
  uint32 inum;
  uint32 new_size;
  struct dinode ip;
  int err = EINVAL;

  task_ctx_clear_errno();
  if(path == 0 || !g_ready){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(length < 0){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if((uint64)length > inode_max_file_size()){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(path_resolve(path, abs_path, sizeof(abs_path)) != 0){
    task_ctx_set_errno(ENOENT);
    return -1;
  }
  new_size = (uint32)length;

  vfs_lock();
  if(path_resolve_final_symlink_locked(abs_path, resolved_path, sizeof(resolved_path)) != 0){
    err = (xv6_last_errno() == ELOOP) ? ELOOP : ENOENT;
    goto out_fail;
  }
  if(is_dev_node(resolved_path)){
    err = EINVAL;
    goto out_fail;
  }
  if(path_lookup(resolved_path, &inum, &ip) != 0){
    err = ENOENT;
    goto out_fail;
  }
  if(ip.type != T_FILE){
    err = EISDIR;
    goto out_fail;
  }
  if(inode_resize(inum, &ip, new_size) != 0){
    err = EIO;
    goto out_fail;
  }
  vfs_unlock();
  return 0;

out_fail:
  task_ctx_set_errno(err);
  vfs_unlock();
  return -1;
}

int xv6_dup(int fd)
{
  int real_fd = stdio_map_fd(fd);
  int nfd = -1;
  int err = EBADF;

  task_ctx_clear_errno();
  if(real_fd < 0 || real_fd >= XV6_MAX_FD){
    task_ctx_set_errno(EBADF);
    return -1;
  }

  vfs_lock();
  if(!g_fds[real_fd].used){
    err = EBADF;
    goto fail;
  }
  nfd = vfs_alloc_fd();
  if(nfd < 0){
    err = EMFILE;
    goto fail;
  }

  g_fds[nfd] = g_fds[real_fd];
  g_fds[nfd].used = 1;
  g_fds[nfd].owner = xTaskGetCurrentTaskHandle();

  if(g_fds[nfd].kind == VFD_PIPE){
    int add_readers = 0;
    int add_writers = 0;
    int id = g_fds[nfd].dev_id;
    if(id < 0 || id >= XV6_MAX_PIPE || !g_pipes[id].alloc){
      err = EIO;
      goto fail_clear;
    }
    if(pipe_endpoints_from_flags(g_fds[nfd].flags, &add_readers, &add_writers) != 0){
      err = EIO;
      goto fail_clear;
    }
    g_pipes[id].readers += add_readers;
    g_pipes[id].writers += add_writers;
  } else if(g_fds[nfd].kind == VFD_DEV){
    int id = g_fds[nfd].dev_id;
    if(g_fds[nfd].dev_role == DEV_ROLE_PTY_MASTER){
      if(id < 0 || id >= XV6_MAX_PTY || !g_ptys[id].alloc){
        err = EIO;
        goto fail_clear;
      }
      g_ptys[id].master_open++;
    } else if(g_fds[nfd].dev_role == DEV_ROLE_PTY_SLAVE){
      if(id < 0 || id >= XV6_MAX_PTY || !g_ptys[id].alloc){
        err = EIO;
        goto fail_clear;
      }
      g_ptys[id].slave_open++;
    }
  }

  vfs_unlock();
  return nfd;

fail_clear:
  memset(&g_fds[nfd], 0, sizeof(g_fds[nfd]));
fail:
  task_ctx_set_errno(err);
  vfs_unlock();
  return -1;
}

int xv6_read(int fd, void *buf, uint32 size)
{
  int real_fd = stdio_map_fd(fd);
  struct dinode ip;
  uint32 cur_off;
  uint32 nread;
  int rc;
  int err = EIO;

  task_ctx_clear_errno();
  if(real_fd < 0 || real_fd >= XV6_MAX_FD){
    task_ctx_set_errno(EBADF);
    return -1;
  }
  if(buf == 0){
    task_ctx_set_errno(EFAULT);
    return -1;
  }
  vfs_lock();
  if(!g_fds[real_fd].used){
    err = EBADF;
    goto fail;
  }
  if((g_fds[real_fd].flags & XV6_O_WRONLY) == XV6_O_WRONLY){
    err = EBADF;
    goto fail;
  }

  if(g_fds[real_fd].kind == VFD_DEV){
    if(g_fds[real_fd].dev_role == DEV_ROLE_PTY_MASTER || g_fds[real_fd].dev_role == DEV_ROLE_PTY_SLAVE){
      rc = dev_read_fd(&g_fds[real_fd], buf, size);
      if(rc < 0){
        err = EIO;
        goto fail;
      }
      vfs_unlock();
      return rc;
    }

    {
      xv6_vfd_t fd_snapshot = g_fds[real_fd];
      vfs_unlock();
      rc = dev_read_fd(&fd_snapshot, buf, size);
    }
    if(rc < 0){
      task_ctx_set_errno(EIO);
      return -1;
    }
    return rc;
  }

  if(g_fds[real_fd].kind == VFD_PIPE){
    uint8 *out = (uint8 *)buf;
    uint32 got = 0;
    int pid = g_fds[real_fd].dev_id;
    if(pid < 0 || pid >= XV6_MAX_PIPE || !g_pipes[pid].alloc){
      err = EBADF;
      goto fail;
    }
    while(got < size){
      int n;
      xv6_pipe_t *p = &g_pipes[pid];
      if(!p->alloc || p->data == 0){
        err = EIO;
        goto fail;
      }
      if(p->n == 0){
        if(p->writers == 0)
          break;
        if(got > 0)
          break;
        vfs_unlock();
        hal_delay_ms(1);
        vfs_lock();
        if(real_fd < 0 || real_fd >= XV6_MAX_FD || !g_fds[real_fd].used || g_fds[real_fd].kind != VFD_PIPE ||
           g_fds[real_fd].dev_id != pid)
        {
          err = EBADF;
          goto fail;
        }
        continue;
      }
      n = pty_q_pop(p->data, &p->r, &p->n, (uint16)XV6_PIPE_BUF_CAP, out + got, size - got);
      if(n <= 0)
        break;
      got += (uint32)n;
    }
    vfs_unlock();
    return (int)got;
  }

  if(read_inode(g_fds[real_fd].inum, &ip) != 0){
    err = EIO;
    goto fail;
  }
  if(ip.type != T_FILE && ip.type != T_DIR){
    err = EBADF;
    goto fail;
  }
  if(ip.type == T_DIR){
    err = EISDIR;
    goto fail;
  }
  cur_off = fd_group_get_off_locked(real_fd);
  if(cur_off >= inode_get_size(&ip)){
    vfs_unlock();
    return 0;
  }
  nread = size;
  if(nread > inode_get_size(&ip) - cur_off)
    nread = inode_get_size(&ip) - cur_off;
  if(nread > 0 && inode_read_range(&ip, cur_off, buf, nread) != 0)
    goto fail;
  fd_group_set_off_locked(real_fd, cur_off + nread);
  vfs_unlock();
  return (int)nread;

fail:
  task_ctx_set_errno(err);
  vfs_unlock();
  return -1;
}

int xv6_write(int fd, const void *buf, uint32 size)
{
  int real_fd = stdio_map_fd(fd);
  struct dinode ip;
  uint32 cur_off;
  int rc;
  int err = EIO;

  task_ctx_clear_errno();
  if(real_fd < 0 || real_fd >= XV6_MAX_FD){
    task_ctx_set_errno(EBADF);
    return -1;
  }
  if(buf == 0){
    task_ctx_set_errno(EFAULT);
    return -1;
  }
  vfs_lock();
  if(!g_fds[real_fd].used){
    err = EBADF;
    goto fail;
  }
  if((g_fds[real_fd].flags & XV6_O_WRONLY) == 0 && (g_fds[real_fd].flags & XV6_O_RDWR) == 0){
    err = EBADF;
    goto fail;
  }

  if(g_fds[real_fd].kind == VFD_DEV){
    rc = dev_write_fd(&g_fds[real_fd], buf, size);
    if(rc < 0){
      err = EIO;
      goto fail;
    }
    vfs_unlock();
    return rc;
  }

  if(g_fds[real_fd].kind == VFD_PIPE){
    const uint8 *in = (const uint8 *)buf;
    uint32 sent = 0;
    int pid = g_fds[real_fd].dev_id;
    if(pid < 0 || pid >= XV6_MAX_PIPE || !g_pipes[pid].alloc){
      err = EBADF;
      goto fail;
    }
    while(sent < size){
      int n;
      xv6_pipe_t *p = &g_pipes[pid];
      if(!p->alloc || p->data == 0){
        err = EIO;
        goto fail;
      }
      if(p->readers == 0){
        if(sent == 0){
          err = EPIPE;
          goto fail;
        }
        break;
      }
      if(p->n >= XV6_PIPE_BUF_CAP){
        if(sent > 0)
          break;
        vfs_unlock();
        hal_delay_ms(1);
        vfs_lock();
        if(real_fd < 0 || real_fd >= XV6_MAX_FD || !g_fds[real_fd].used || g_fds[real_fd].kind != VFD_PIPE ||
           g_fds[real_fd].dev_id != pid)
        {
          err = EBADF;
          goto fail;
        }
        continue;
      }
      n = pty_q_push(p->data, &p->w, &p->n, (uint16)XV6_PIPE_BUF_CAP, in + sent, size - sent);
      if(n <= 0)
        break;
      sent += (uint32)n;
    }
    vfs_unlock();
    return (int)sent;
  }

  if(read_inode(g_fds[real_fd].inum, &ip) != 0){
    err = EIO;
    goto fail;
  }
  if(ip.type != T_FILE){
    err = EISDIR;
    goto fail;
  }
  cur_off = fd_group_get_off_locked(real_fd);
  if((g_fds[real_fd].flags & XV6_O_APPEND) != 0){
    cur_off = inode_get_size(&ip);
    fd_group_set_off_locked(real_fd, cur_off);
  }
  if(inode_write_range(&ip, cur_off, buf, size) != 0){
    err = EIO;
    goto fail;
  }
  if(write_inode(g_fds[real_fd].inum, &ip) != 0){
    err = EIO;
    goto fail;
  }
  fd_group_set_off_locked(real_fd, cur_off + size);
  vfs_unlock();
  return (int)size;

fail:
  task_ctx_set_errno(err);
  vfs_unlock();
  return -1;
}

static void vfs_close_fd_locked(int real_fd)
{
  if(real_fd < 0 || real_fd >= XV6_MAX_FD || !g_fds[real_fd].used || real_fd <= 2)
    return;
  if(g_fds[real_fd].kind == VFD_DEV){
    int id = g_fds[real_fd].dev_id;
    if(g_fds[real_fd].dev_role == DEV_ROLE_PTY_MASTER && id >= 0 && id < XV6_MAX_PTY){
      if(g_ptys[id].master_open > 0)
        g_ptys[id].master_open--;
      pty_drop_queued_data_if_orphaned(id);
      pty_try_free(id);
    } else if(g_fds[real_fd].dev_role == DEV_ROLE_PTY_SLAVE && id >= 0 && id < XV6_MAX_PTY){
      if(g_ptys[id].slave_open > 0)
        g_ptys[id].slave_open--;
      pty_drop_queued_data_if_orphaned(id);
      pty_try_free(id);
    }
  } else if(g_fds[real_fd].kind == VFD_PIPE){
    int rem_readers = 0;
    int rem_writers = 0;
    int id = g_fds[real_fd].dev_id;
    if(id >= 0 && id < XV6_MAX_PIPE && g_pipes[id].alloc){
      if(pipe_endpoints_from_flags(g_fds[real_fd].flags, &rem_readers, &rem_writers) == 0){
        g_pipes[id].readers -= rem_readers;
        g_pipes[id].writers -= rem_writers;
      }
      if(g_pipes[id].writers < 0)
        g_pipes[id].writers = 0;
      if(g_pipes[id].readers < 0)
        g_pipes[id].readers = 0;
      pipe_try_free(id);
      if(!g_pipes[id].alloc)
        fifo_unbind_pipe_locked(id);
    }
  }
  memset(&g_fds[real_fd], 0, sizeof(g_fds[real_fd]));
}

int xv6_close(int fd)
{
  int real_fd = stdio_map_fd(fd);

  task_ctx_clear_errno();
  if(real_fd < 0 || real_fd >= XV6_MAX_FD){
    task_ctx_set_errno(EBADF);
    return -1;
  }
  vfs_lock();
  if(!g_fds[real_fd].used || real_fd <= 2){
    task_ctx_set_errno(EBADF);
    vfs_unlock();
    return -1;
  }
  vfs_close_fd_locked(real_fd);
  vfs_unlock();
  return 0;
}

static void fill_kstat_from_inode_locked(uint32 inum, const struct dinode *ip, xv6_kstat_t *st)
{
  st->ino = inum;
  st->size = inode_get_size(ip);
  st->type = ip->type;
  st->nlink = (uint16)ip->nlink;
  st->mode = inode_get_mode(ip);
  st->uid = (uint16)inode_get_uid(ip);
  st->gid = (uint16)inode_get_gid(ip);
}

int xv6_stat_path(const char *path, xv6_kstat_t *st)
{
  char abs_path[VFS_PATH_MAX];
  char resolved_path[VFS_PATH_MAX];
  uint32 inum;
  struct dinode ip;
  int err = EIO;

  task_ctx_clear_errno();
  if(path == 0 || st == 0 || !g_ready){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(path_resolve(path, abs_path, sizeof(abs_path)) != 0){
    task_ctx_set_errno(ENOENT);
    return -1;
  }

  memset(st, 0, sizeof(*st));

  if(is_dev_node(abs_path)){
    st->type = T_DEVICE;
    st->nlink = 1;
    st->mode = (uint16)XV6_DEFAULT_DEV_MODE;
    st->uid = 0;
    st->gid = 0;
    return 0;
  }

  vfs_lock();
  if(path_resolve_final_symlink_locked(abs_path, resolved_path, sizeof(resolved_path)) != 0){
    err = (xv6_last_errno() == ELOOP) ? ELOOP : ENOENT;
    goto out_fail;
  }
  if(is_dev_node(resolved_path)){
    st->type = T_DEVICE;
    st->nlink = 1;
    st->mode = (uint16)XV6_DEFAULT_DEV_MODE;
    st->uid = 0;
    st->gid = 0;
    vfs_unlock();
    return 0;
  }
  if(path_lookup(resolved_path, &inum, &ip) != 0){
    err = ENOENT;
    goto out_fail;
  }
  fill_kstat_from_inode_locked(inum, &ip, st);
  vfs_unlock();
  return 0;

out_fail:
  task_ctx_set_errno(err);
  vfs_unlock();
  return -1;
}

int xv6_lstat_path(const char *path, xv6_kstat_t *st)
{
  char abs_path[VFS_PATH_MAX];
  uint32 inum;
  struct dinode ip;

  task_ctx_clear_errno();
  if(path == 0 || st == 0 || !g_ready){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(path_resolve(path, abs_path, sizeof(abs_path)) != 0){
    task_ctx_set_errno(ENOENT);
    return -1;
  }

  memset(st, 0, sizeof(*st));

  if(is_dev_node(abs_path)){
    st->type = T_DEVICE;
    st->nlink = 1;
    st->mode = (uint16)XV6_DEFAULT_DEV_MODE;
    st->uid = 0;
    st->gid = 0;
    return 0;
  }

  vfs_lock();
  if(path_lookup(abs_path, &inum, &ip) != 0){
    task_ctx_set_errno(ENOENT);
    vfs_unlock();
    return -1;
  }
  fill_kstat_from_inode_locked(inum, &ip, st);
  vfs_unlock();
  return 0;
}

int xv6_fstat(int fd, xv6_kstat_t *st)
{
  int real_fd = stdio_map_fd(fd);
  struct dinode ip;

  task_ctx_clear_errno();
  if(st == 0){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(real_fd < 0 || real_fd >= XV6_MAX_FD){
    task_ctx_set_errno(EBADF);
    return -1;
  }

  memset(st, 0, sizeof(*st));

  vfs_lock();
  if(!g_fds[real_fd].used){
    task_ctx_set_errno(EBADF);
    vfs_unlock();
    return -1;
  }
  if(g_fds[real_fd].kind == VFD_DEV){
    st->type = T_DEVICE;
    st->nlink = 1;
    st->mode = (uint16)XV6_DEFAULT_DEV_MODE;
    st->uid = 0;
    st->gid = 0;
    vfs_unlock();
    return 0;
  }
  if(g_fds[real_fd].kind == VFD_PIPE){
    if(g_fds[real_fd].inum != 0 && read_inode(g_fds[real_fd].inum, &ip) == 0 && ip.type == T_FIFO){
      fill_kstat_from_inode_locked(g_fds[real_fd].inum, &ip, st);
      vfs_unlock();
      return 0;
    }
    st->type = T_FIFO;
    st->nlink = 1;
    st->mode = (uint16)XV6_DEFAULT_FIFO_MODE;
    st->uid = 0;
    st->gid = 0;
    vfs_unlock();
    return 0;
  }
  if(g_fds[real_fd].kind != VFD_FILE){
    task_ctx_set_errno(EBADF);
    vfs_unlock();
    return -1;
  }
  if(read_inode(g_fds[real_fd].inum, &ip) != 0){
    task_ctx_set_errno(EIO);
    vfs_unlock();
    return -1;
  }
  fill_kstat_from_inode_locked(g_fds[real_fd].inum, &ip, st);
  vfs_unlock();
  return 0;
}

int xv6_fd_path(int fd, char *out_path, int out_len)
{
  int real_fd = stdio_map_fd(fd);
  int err = EIO;

  task_ctx_clear_errno();
  if(out_path == 0 || out_len <= 1){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(real_fd < 0 || real_fd >= XV6_MAX_FD){
    task_ctx_set_errno(EBADF);
    return -1;
  }

  vfs_lock();
  if(!g_fds[real_fd].used){
    err = EBADF;
    goto out_fail;
  }
  if(g_fds[real_fd].path[0] == 0){
    err = ENOENT;
    goto out_fail;
  }
  if((int)strlen(g_fds[real_fd].path) >= out_len){
    err = ERANGE;
    goto out_fail;
  }

  copy_cstr(out_path, out_len, g_fds[real_fd].path);
  vfs_unlock();
  return 0;

out_fail:
  task_ctx_set_errno(err);
  vfs_unlock();
  return -1;
}

int xv6_access(const char *path, int mode)
{
  const int mode_r = 4;
  const int mode_w = 2;
  const int mode_x = 1;
  const int mode_mask = mode_r | mode_w | mode_x;
  xv6_kstat_t st;

  task_ctx_clear_errno();
  if((mode & ~mode_mask) != 0){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(xv6_stat_path(path, &st) != 0)
    return -1;
  if(mode == 0)
    return 0;
  if((mode & mode_r) != 0 && (st.mode & 0444u) == 0u){
    task_ctx_set_errno(EACCES);
    return -1;
  }
  if((mode & mode_w) != 0 && (st.mode & 0222u) == 0u){
    task_ctx_set_errno(EACCES);
    return -1;
  }
  if((mode & mode_x) != 0 && (st.mode & 0111u) == 0u){
    task_ctx_set_errno(EACCES);
    return -1;
  }
  return 0;
}

int xv6_chmod(const char *path, int mode)
{
  char abs_path[VFS_PATH_MAX];
  char resolved_path[VFS_PATH_MAX];
  uint32 inum;
  struct dinode ip;
  int err = EIO;

  task_ctx_clear_errno();
  if(path == 0 || !g_ready){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(path_resolve(path, abs_path, sizeof(abs_path)) != 0){
    task_ctx_set_errno(ENOENT);
    return -1;
  }
  if(is_dev_node(abs_path))
    return 0;

  vfs_lock();
  if(path_resolve_final_symlink_locked(abs_path, resolved_path, sizeof(resolved_path)) != 0){
    err = (xv6_last_errno() == ELOOP) ? ELOOP : ENOENT;
    goto out_fail;
  }
  if(is_dev_node(resolved_path)){
    vfs_unlock();
    return 0;
  }
  if(path_lookup(resolved_path, &inum, &ip) != 0){
    err = ENOENT;
    goto out_fail;
  }

  inode_set_mode(&ip, (uint16)mode);
  if(write_inode(inum, &ip) != 0){
    err = EIO;
    goto out_fail;
  }
  vfs_unlock();
  return 0;

out_fail:
  task_ctx_set_errno(err);
  vfs_unlock();
  return -1;
}

int xv6_chown_path(const char *path, int owner, int group, int follow_final_nonzero)
{
  char abs_path[VFS_PATH_MAX];
  char lookup_path[VFS_PATH_MAX];
  uint32 inum;
  struct dinode ip;
  int err = EIO;

  task_ctx_clear_errno();
  if(path == 0 || !g_ready){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(owner < -1 || group < -1){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(owner > 0xffff || group > 0xffff){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(path_resolve(path, abs_path, sizeof(abs_path)) != 0){
    task_ctx_set_errno(ENOENT);
    return -1;
  }
  if(is_dev_node(abs_path))
    return 0;

  vfs_lock();
  if(follow_final_nonzero){
    if(path_resolve_final_symlink_locked(abs_path, lookup_path, sizeof(lookup_path)) != 0){
      err = (xv6_last_errno() == ELOOP) ? ELOOP : ENOENT;
      goto out_fail;
    }
  } else {
    copy_cstr(lookup_path, sizeof(lookup_path), abs_path);
  }

  if(is_dev_node(lookup_path)){
    vfs_unlock();
    return 0;
  }
  if(path_lookup(lookup_path, &inum, &ip) != 0){
    err = ENOENT;
    goto out_fail;
  }

  inode_set_uid_gid(&ip, owner, group);
  if(write_inode(inum, &ip) != 0){
    err = EIO;
    goto out_fail;
  }
  vfs_unlock();
  return 0;

out_fail:
  task_ctx_set_errno(err);
  vfs_unlock();
  return -1;
}

int xv6_fchmod(int fd, int mode)
{
  int real_fd = stdio_map_fd(fd);
  uint32 inum;
  struct dinode ip;
  int err = EIO;

  task_ctx_clear_errno();
  if(real_fd < 0 || real_fd >= XV6_MAX_FD){
    task_ctx_set_errno(EBADF);
    return -1;
  }

  vfs_lock();
  if(!g_fds[real_fd].used){
    err = EBADF;
    goto out_fail;
  }
  if(g_fds[real_fd].kind == VFD_DEV){
    vfs_unlock();
    return 0;
  }
  if(g_fds[real_fd].kind != VFD_FILE){
    err = EBADF;
    goto out_fail;
  }
  inum = g_fds[real_fd].inum;
  if(read_inode(inum, &ip) != 0){
    err = EIO;
    goto out_fail;
  }
  inode_set_mode(&ip, (uint16)mode);
  if(write_inode(inum, &ip) != 0){
    err = EIO;
    goto out_fail;
  }

  vfs_unlock();
  return 0;

out_fail:
  task_ctx_set_errno(err);
  vfs_unlock();
  return -1;
}

int xv6_fchown(int fd, int owner, int group)
{
  int real_fd = stdio_map_fd(fd);
  uint32 inum;
  struct dinode ip;
  int err = EIO;

  task_ctx_clear_errno();
  if(real_fd < 0 || real_fd >= XV6_MAX_FD){
    task_ctx_set_errno(EBADF);
    return -1;
  }
  if(owner < -1 || group < -1){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(owner > 0xffff || group > 0xffff){
    task_ctx_set_errno(EINVAL);
    return -1;
  }

  vfs_lock();
  if(!g_fds[real_fd].used){
    err = EBADF;
    goto out_fail;
  }
  if(g_fds[real_fd].kind == VFD_DEV){
    vfs_unlock();
    return 0;
  }
  if(g_fds[real_fd].kind != VFD_FILE){
    err = EBADF;
    goto out_fail;
  }
  inum = g_fds[real_fd].inum;
  if(read_inode(inum, &ip) != 0){
    err = EIO;
    goto out_fail;
  }
  inode_set_uid_gid(&ip, owner, group);
  if(write_inode(inum, &ip) != 0){
    err = EIO;
    goto out_fail;
  }

  vfs_unlock();
  return 0;

out_fail:
  task_ctx_set_errno(err);
  vfs_unlock();
  return -1;
}

int xv6_chdir(const char *path)
{
  char abs_path[VFS_PATH_MAX];
  char resolved_path[VFS_PATH_MAX];
  uint32 inum;
  struct dinode ip;
  xv6_task_ctx_t *ctx;
  int err = EIO;

  task_ctx_clear_errno();
  if(path == 0 || !g_ready){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  if(path_resolve(path, abs_path, sizeof(abs_path)) != 0){
    task_ctx_set_errno(ENOENT);
    return -1;
  }

  vfs_lock();
  if(path_resolve_final_symlink_locked(abs_path, resolved_path, sizeof(resolved_path)) != 0){
    err = (xv6_last_errno() == ELOOP) ? ELOOP : ENOENT;
    goto out_fail;
  }
  if(is_dev_node(resolved_path)){
    err = ENOTDIR;
    goto out_fail;
  }
  if(path_lookup(resolved_path, &inum, &ip) != 0 || ip.type != T_DIR){
    err = ENOTDIR;
    goto out_fail;
  }
  vfs_unlock();

  ctx = task_ctx_get(1);
  if(ctx == 0){
    task_ctx_set_errno(EIO);
    return -1;
  }
  copy_cstr(ctx->cwd, sizeof(ctx->cwd), resolved_path);
  return 0;

out_fail:
  task_ctx_set_errno(err);
  vfs_unlock();
  return -1;
}

int xv6_getcwd(char *out_path, int out_len)
{
  xv6_task_ctx_t *ctx;

  task_ctx_clear_errno();
  if(out_path == 0 || out_len <= 1){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  ctx = task_ctx_get(1);
  if(ctx == 0){
    task_ctx_set_errno(EIO);
    return -1;
  }
  if((int)strlen(ctx->cwd) >= out_len){
    task_ctx_set_errno(ERANGE);
    return -1;
  }
  copy_cstr(out_path, out_len, ctx->cwd);
  return 0;
}

int xv6_ptsname(int master_fd, char *out_path, int out_len)
{
  int id;
  task_ctx_clear_errno();
  if(out_path == 0 || out_len <= 0){
    task_ctx_set_errno(EINVAL);
    return -1;
  }
  vfs_lock();
  if(master_fd < 0 || master_fd >= XV6_MAX_FD || !g_fds[master_fd].used){
    task_ctx_set_errno(EBADF);
    goto fail;
  }
  if(g_fds[master_fd].kind != VFD_DEV || g_fds[master_fd].dev_role != DEV_ROLE_PTY_MASTER){
    task_ctx_set_errno(EINVAL);
    goto fail;
  }
  id = g_fds[master_fd].dev_id;
  if(id < 0 || id >= XV6_MAX_PTY || !g_ptys[id].alloc){
    task_ctx_set_errno(ENOENT);
    goto fail;
  }
  {
    int n = snprintf(out_path, out_len, "/dev/pts/%d", id);
    if(n <= 0 || n >= out_len){
      task_ctx_set_errno(ERANGE);
      goto fail;
    }
  }
  vfs_unlock();
  return 0;

fail:
  vfs_unlock();
  return -1;
}

int xv6_tty_flush_input(int fd)
{
  int real_fd = stdio_map_fd(fd);

  task_ctx_clear_errno();
  if(real_fd < 0 || real_fd >= XV6_MAX_FD){
    task_ctx_set_errno(EBADF);
    return -1;
  }

  vfs_lock();
  if(!g_fds[real_fd].used){
    task_ctx_set_errno(EBADF);
    vfs_unlock();
    return -1;
  }
  if(g_fds[real_fd].kind != VFD_DEV){
    task_ctx_set_errno(ENOTTY);
    vfs_unlock();
    return -1;
  }
  if(g_fds[real_fd].dev_role == DEV_ROLE_PTY_MASTER || g_fds[real_fd].dev_role == DEV_ROLE_PTY_SLAVE){
    int id = g_fds[real_fd].dev_id;

    if(id < 0 || id >= XV6_MAX_PTY || !g_ptys[id].alloc){
      task_ctx_set_errno(ENOTTY);
      vfs_unlock();
      return -1;
    }
    if(g_fds[real_fd].dev_role == DEV_ROLE_PTY_MASTER)
      pty_q_drop_all(&g_ptys[id].s2m_r, &g_ptys[id].s2m_w, &g_ptys[id].s2m_n);
    else
      pty_q_drop_all(&g_ptys[id].m2s_r, &g_ptys[id].m2s_w, &g_ptys[id].m2s_n);
    vfs_unlock();
    return 0;
  }
  if(strcmp(g_fds[real_fd].path, "/dev/tty") == 0 || strcmp(g_fds[real_fd].path, "/dev/stdin") == 0 ||
     strcmp(g_fds[real_fd].path, "/dev/stdout") == 0 || strcmp(g_fds[real_fd].path, "/dev/stderr") == 0 ||
     strcmp(g_fds[real_fd].path, "/dev/console") == 0)
  {
    vfs_unlock();
    hal_console_discard_input();
    return 0;
  }

  task_ctx_set_errno(ENOTTY);
  vfs_unlock();
  return -1;
}

int xv6_pipe(int *out_read_fd, int *out_write_fd)
{
  int pipe_id;
  int rfd, wfd;

  task_ctx_clear_errno();
  if(out_read_fd == 0 || out_write_fd == 0 || !g_ready){
    task_ctx_set_errno(EINVAL);
    return -1;
  }

  vfs_lock();
  pipe_id = pipe_alloc_id();
  if(pipe_id < 0){
    task_ctx_set_errno(ENFILE);
    goto fail;
  }

  rfd = vfs_alloc_fd();
  if(rfd < 0){
    task_ctx_set_errno(EMFILE);
    goto fail_pipe;
  }
  memset(&g_fds[rfd], 0, sizeof(g_fds[rfd]));
  g_fds[rfd].used = 1;
  g_fds[rfd].kind = VFD_PIPE;
  g_fds[rfd].flags = XV6_O_RDONLY;
  g_fds[rfd].dev_id = pipe_id;
  g_fds[rfd].group_id = 0;
  g_fds[rfd].owner = xTaskGetCurrentTaskHandle();

  wfd = vfs_alloc_fd();
  if(wfd < 0){
    task_ctx_set_errno(EMFILE);
    memset(&g_fds[rfd], 0, sizeof(g_fds[rfd]));
    goto fail_pipe;
  }
  memset(&g_fds[wfd], 0, sizeof(g_fds[wfd]));
  g_fds[wfd].used = 1;
  g_fds[wfd].kind = VFD_PIPE;
  g_fds[wfd].flags = XV6_O_WRONLY;
  g_fds[wfd].dev_id = pipe_id;
  g_fds[wfd].group_id = 0;
  g_fds[wfd].owner = xTaskGetCurrentTaskHandle();

  g_pipes[pipe_id].readers = 1;
  g_pipes[pipe_id].writers = 1;
  *out_read_fd = rfd;
  *out_write_fd = wfd;
  vfs_unlock();
  return 0;

fail_pipe:
  pipe_slot_reset(&g_pipes[pipe_id]);
fail:
  vfs_unlock();
  return -1;
}

int xv6_stdio_set_fds(int in_fd, int out_fd, int err_fd)
{
  xv6_task_ctx_t *ctx = task_ctx_get(1);
  if(ctx == 0){
    task_ctx_set_errno(ENFILE);
    return -1;
  }
  ctx->stdio_active = 1;
  ctx->in_fd = in_fd;
  ctx->out_fd = out_fd;
  ctx->err_fd = err_fd;
  return 0;
}

void xv6_stdio_reset_fds(void)
{
  xv6_task_ctx_t *ctx = task_ctx_get(0);
  if(ctx){
    ctx->stdio_active = 0;
    ctx->in_fd = 0;
    ctx->out_fd = 1;
    ctx->err_fd = 2;
  }
}

void xv6_stdio_get_fds(int *out_in_fd, int *out_out_fd, int *out_err_fd, int *out_active)
{
  xv6_task_ctx_t *ctx = task_ctx_get(0);
  int active = 0;
  int in_fd = 0;
  int out_fd = 1;
  int err_fd = 2;

  if(ctx && ctx->stdio_active){
    active = 1;
    in_fd = ctx->in_fd;
    out_fd = ctx->out_fd;
    err_fd = ctx->err_fd;
  }

  if(out_in_fd)
    *out_in_fd = in_fd;
  if(out_out_fd)
    *out_out_fd = out_fd;
  if(out_err_fd)
    *out_err_fd = err_fd;
  if(out_active)
    *out_active = active;
}

int xv6_stdio_is_default_out(void)
{
  xv6_task_ctx_t *ctx = task_ctx_get(0);
  if(ctx == 0 || !ctx->stdio_active)
    return 1;
  return (ctx->out_fd == 1 && ctx->err_fd == 2) ? 1 : 0;
}

void xv6_task_ctx_cleanup(void)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  xv6_task_ctx_cleanup_for_handle((void *)self);
}

void xv6_task_ctx_cleanup_for_handle(void *task_handle)
{
  TaskHandle_t self = (TaskHandle_t)task_handle;
  int i;

  if(self == 0)
    return;

  elf_loader_task_cleanup_for_handle(task_handle);

  vfs_lock();
  if(g_fds){
    for(i = 3; i < XV6_MAX_FD; i++){
      if(!g_fds[i].used || g_fds[i].owner != self)
        continue;
      vfs_close_fd_locked(i);
    }
  }
  vfs_unlock();

  task_ctx_lock();
  for(i = 0; i < g_task_ctx_cap; i++){
    if(g_task_ctx[i].task == self){
      memset(&g_task_ctx[i], 0, sizeof(g_task_ctx[i]));
      break;
    }
  }
  task_ctx_unlock();
}

int xv6fs_ro_list(int index, char *name_out, int name_out_len, uint32 *size_out)
{
  return xv6fs_list_path("/", index, name_out, name_out_len, 0, size_out);
}

int xv6fs_ro_read_file_alloc(const char *name, void **out_data, uint32 *out_size)
{
  char path[VFS_PATH_MAX];
  int n;
  if(name == 0)
    return -1;
  if(name[0] == '/')
    return xv6fs_read_file_alloc_path(name, out_data, out_size);
  n = snprintf(path, sizeof(path), "/%s", name);
  if(n <= 0 || n >= (int)sizeof(path))
    return -1;
  return xv6fs_read_file_alloc_path(path, out_data, out_size);
}
