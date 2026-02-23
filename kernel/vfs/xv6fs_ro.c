#include "vfs/xv6fs_ro.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "platform/esp_flash_disk.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "loader/elf_loader.h"
#include "fs/fs.h"
#include "platform/hal.h"
#include "core/param.h"
#include "fs/stat.h"

static const char *TAG = "xv6fs";

static struct superblock g_sb;
static int g_ready;
static uint32 g_nbitmap;
static uint32 g_data_start;
static SemaphoreHandle_t g_vfs_lock;
static SemaphoreHandle_t g_ctx_lock;

typedef struct {
  int stdio_active;
  int in_fd;
  int out_fd;
  int err_fd;
  int last_errno;
  char cwd[MAXPATH];
} xv6_task_ctx_t;

#define XV6_MAX_TASK_CTX XV6_TASK_CTX_CAP

typedef struct {
  TaskHandle_t task;
  xv6_task_ctx_t ctx;
} xv6_task_ctx_slot_t;

static xv6_task_ctx_slot_t g_task_ctx[XV6_MAX_TASK_CTX];

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

static xv6_vfd_t g_fds[XV6_MAX_FD];
static int g_next_fd_group = 1;

static void copy_cstr(char *dst, int dst_len, const char *src)
{
  if(dst == 0 || dst_len <= 0)
    return;
  if(src == 0)
    src = "";
  strncpy(dst, src, (size_t)dst_len - 1u);
  dst[dst_len - 1] = 0;
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
  uint8 m2s[256];
  uint16 m2s_r;
  uint16 m2s_w;
  uint16 m2s_n;
  uint8 s2m[256];
  uint16 s2m_r;
  uint16 s2m_w;
  uint16 s2m_n;
} xv6_pty_t;

static xv6_pty_t g_ptys[XV6_MAX_PTY];

typedef struct {
  int alloc;
  int readers;
  int writers;
  uint8 data[512];
  uint16 r;
  uint16 w;
  uint16 n;
} xv6_pipe_t;

static xv6_pipe_t g_pipes[XV6_MAX_PIPE];

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

  for(i = 0; i < XV6_MAX_TASK_CTX; i++){
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
  for(i = 0; i < XV6_MAX_TASK_CTX; i++){
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
  char path_buf[MAXPATH];
  const char *p;
  const char *prefix = "/";
  xv6_task_ctx_t *ctx;
  int stack[32];
  int nseg = 0;

  if(path == 0 || out == 0 || out_len <= 1)
    return -1;
  if(copy_guest_cstr(path, path_buf, sizeof(path_buf)) != 0)
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
    stack[nseg++] = (int)(seg - path) | 0x40000000;
  }

  {
    int i;
    int pos = 0;
    out[pos++] = '/';
    for(i = 0; i < nseg; i++){
      const char *src;
      int len = 0;
      int idx = stack[i];
      if((idx & 0x40000000) != 0){
        src = path + (idx & 0x3fffffff);
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

static int is_dev_node(const char *path)
{
  char canon[MAXPATH];
  if(dev_canonical_path(path, canon, sizeof(canon)) != 0)
    return 0;
  return strncmp(canon, "/dev/", 5) == 0 || strcmp(canon, "/dev") == 0;
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

static int pty_alloc_id(void)
{
  int i;
  for(i = 0; i < XV6_MAX_PTY; i++){
    if(!g_ptys[i].alloc){
      memset(&g_ptys[i], 0, sizeof(g_ptys[i]));
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
    memset(&g_ptys[id], 0, sizeof(g_ptys[id]));
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
      memset(&g_pipes[i], 0, sizeof(g_pipes[i]));
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
    memset(&g_pipes[id], 0, sizeof(g_pipes[id]));
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
  char canon[MAXPATH];
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
  char canon[MAXPATH];
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
    if(fd->dev_role == DEV_ROLE_PTY_MASTER)
      return pty_q_pop(p->s2m, &p->s2m_r, &p->s2m_n, sizeof(p->s2m), (uint8 *)buf, size);
    return pty_q_pop(p->m2s, &p->m2s_r, &p->m2s_n, sizeof(p->m2s), (uint8 *)buf, size);
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
    if(fd->dev_role == DEV_ROLE_PTY_MASTER)
      return pty_q_push(p->m2s, &p->m2s_w, &p->m2s_n, sizeof(p->m2s), (const uint8 *)buf, size);
    return pty_q_push(p->s2m, &p->s2m_w, &p->s2m_n, sizeof(p->s2m), (const uint8 *)buf, size);
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

  if(ip == 0 || dst == 0)
    return -1;
  if(off > ip->size)
    return -1;
  if(n > ip->size - off)
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
      ip.size = 0;
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

  if(ip == 0 || src == 0)
    return -1;
  if(n == 0)
    return 0;
  if(off > (uint32)0xffffffffu - n)
    return -1;
  end_off = off + n;
  if(end_off > max_file_size)
    return -1;

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
  if(end_off > ip->size)
    ip->size = end_off;
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
  ip->size = 0;
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

  if(read_inode(dir_inum, &dir) != 0 || dir.type != T_DIR)
    return -1;

  for(off = 0; off + sizeof(struct dirent) <= dir.size; off += sizeof(struct dirent)){
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

static int dir_add_entry(uint32 dir_inum, const char *name, uint32 inum)
{
  struct dinode dir;
  uint32 off;
  struct dirent de;

  if(strlen(name) > DIRSIZ)
    return -1;
  if(read_inode(dir_inum, &dir) != 0 || dir.type != T_DIR)
    return -1;

  for(off = 0; off + sizeof(de) <= dir.size; off += sizeof(de)){
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

  if(read_inode(dir_inum, &dir) != 0 || dir.type != T_DIR)
    return -1;

  for(off = 0; off + sizeof(struct dirent) <= dir.size; off += sizeof(struct dirent)){
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

  if(in_de == 0)
    return -1;
  if(read_inode(dir_inum, &dir) != 0 || dir.type != T_DIR)
    return -1;
  if(off + sizeof(*in_de) > dir.size)
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

  if(read_inode(dir_inum, &dir) != 0 || dir.type != T_DIR)
    return -1;
  for(off = 0; off + sizeof(struct dirent) <= dir.size; off += sizeof(struct dirent)){
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
  g_ready = 1;
  xv6_vfs_reset();
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
  char abs_path[MAXPATH];
  uint32 dir_inum;
  struct dinode dir;
  uint32 off;
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

  for(off = 0; off + sizeof(struct dirent) <= dir.size; off += sizeof(struct dirent)){
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
      *size_out = ent.size;
    rc = 0;
    goto out;
  }

out:
  vfs_unlock();
  if(rc != 0)
    task_ctx_set_errno(err);
  return rc;
}

int xv6fs_read_file_alloc_path(const char *path, void **out_data, uint32 *out_size)
{
  char abs_path[MAXPATH];
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

  buf = malloc(ip.size ? ip.size : 1);
  if(buf == 0){
    err = ENOMEM;
    rc = -1;
    goto out_unlock;
  }
  if(ip.size > 0 && inode_read_range(&ip, 0, buf, ip.size) != 0){
    free(buf);
    err = EIO;
    rc = -1;
    goto out_unlock;
  }
  *out_data = buf;
  *out_size = ip.size;
  rc = 0;

out_unlock:
  vfs_unlock();
  if(rc != 0)
    task_ctx_set_errno(err);
  return rc;
}

int xv6fs_mkdir_path(const char *path)
{
  char abs_path[MAXPATH];
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

int xv6fs_write_file_path(const char *path, const void *data, uint32 size)
{
  char abs_path[MAXPATH];
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
  char abs_path[MAXPATH];
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
  if(ip.type != T_FILE){
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
  char abs_path[MAXPATH];
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
  char old_abs[MAXPATH];
  char new_abs[MAXPATH];
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

static int vfs_alloc_fd(void)
{
  int i;
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

  if(path_parent(path, &pinum, name) != 0)
    return -1;
  if(dir_lookup_inum(pinum, name, &inum, &ip) == 0){
    if(ip.type != T_FILE)
      return -1;
    *out_inum = inum;
    return 0;
  }

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
  if(g_vfs_lock == 0)
    g_vfs_lock = xSemaphoreCreateMutex();
  vfs_lock();
  memset(g_fds, 0, sizeof(g_fds));
  memset(g_ptys, 0, sizeof(g_ptys));
  memset(g_pipes, 0, sizeof(g_pipes));
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
  char abs_path[MAXPATH];
  char canon[MAXPATH];
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
    g_fds[fd].off = ip.size;
  }
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
    base_off = (long long)ip.size;
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
    int id = g_fds[nfd].dev_id;
    if(id < 0 || id >= XV6_MAX_PIPE || !g_pipes[id].alloc){
      err = EIO;
      goto fail_clear;
    }
    if((g_fds[nfd].flags & XV6_O_WRONLY) != 0)
      g_pipes[id].writers++;
    else
      g_pipes[id].readers++;
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
    rc = dev_read_fd(&g_fds[real_fd], buf, size);
    if(rc < 0){
      err = EIO;
      goto fail;
    }
    if(rc > 0)
      g_fds[real_fd].off += (uint32)rc;
    vfs_unlock();
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
      if(!p->alloc){
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
      n = pty_q_pop(p->data, &p->r, &p->n, sizeof(p->data), out + got, size - got);
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
  if(cur_off >= ip.size){
    vfs_unlock();
    return 0;
  }
  nread = size;
  if(nread > ip.size - cur_off)
    nread = ip.size - cur_off;
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
      if(!p->alloc){
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
      if(p->n >= sizeof(p->data)){
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
      n = pty_q_push(p->data, &p->w, &p->n, sizeof(p->data), in + sent, size - sent);
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
    cur_off = ip.size;
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
    int id = g_fds[real_fd].dev_id;
    if(id >= 0 && id < XV6_MAX_PIPE && g_pipes[id].alloc){
      if((g_fds[real_fd].flags & XV6_O_WRONLY) != 0)
        g_pipes[id].writers--;
      else
        g_pipes[id].readers--;
      if(g_pipes[id].writers < 0)
        g_pipes[id].writers = 0;
      if(g_pipes[id].readers < 0)
        g_pipes[id].readers = 0;
      pipe_try_free(id);
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

int xv6_stat_path(const char *path, xv6_kstat_t *st)
{
  char abs_path[MAXPATH];
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
    return 0;
  }

  vfs_lock();
  if(path_lookup(abs_path, &inum, &ip) != 0){
    task_ctx_set_errno(ENOENT);
    vfs_unlock();
    return -1;
  }
  st->ino = inum;
  st->size = ip.size;
  st->type = ip.type;
  st->nlink = (uint16)ip.nlink;
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
  st->ino = g_fds[real_fd].inum;
  st->size = ip.size;
  st->type = ip.type;
  st->nlink = (uint16)ip.nlink;
  vfs_unlock();
  return 0;
}

int xv6_access(const char *path, int mode)
{
  xv6_kstat_t st;
  (void)mode;
  if(xv6_stat_path(path, &st) != 0)
    return -1;
  return 0;
}

int xv6_chmod(const char *path, int mode)
{
  xv6_kstat_t st;
  (void)mode;
  if(xv6_stat_path(path, &st) != 0)
    return -1;
  return 0;
}

int xv6_chdir(const char *path)
{
  char abs_path[MAXPATH];
  uint32 inum;
  struct dinode ip;
  xv6_task_ctx_t *ctx;

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
    task_ctx_set_errno(ENOTDIR);
    return -1;
  }

  vfs_lock();
  if(path_lookup(abs_path, &inum, &ip) != 0 || ip.type != T_DIR){
    task_ctx_set_errno(ENOTDIR);
    vfs_unlock();
    return -1;
  }
  vfs_unlock();

  ctx = task_ctx_get(1);
  if(ctx == 0){
    task_ctx_set_errno(EIO);
    return -1;
  }
  copy_cstr(ctx->cwd, sizeof(ctx->cwd), abs_path);
  return 0;
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
  memset(&g_pipes[pipe_id], 0, sizeof(g_pipes[pipe_id]));
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
  for(i = 3; i < XV6_MAX_FD; i++){
    if(!g_fds[i].used || g_fds[i].owner != self)
      continue;
    vfs_close_fd_locked(i);
  }
  vfs_unlock();

  task_ctx_lock();
  for(i = 0; i < XV6_MAX_TASK_CTX; i++){
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
  char path[MAXPATH];
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
