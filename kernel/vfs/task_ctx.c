/**
 * @file task_ctx.c
 * @brief Task context implementation
 */
#include "vfs/task_ctx.h"

#include <errno.h>
#include <string.h>

#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "loader/elf_loader.h"
#include "vfs/xv6fs_ro.h"

#define XV6_MAX_TASK_CTX XV6_TASK_CTX_CAP

typedef struct {
  TaskHandle_t task;
  xv6_task_ctx_t ctx;
} xv6_task_ctx_slot_t;

static xv6_task_ctx_slot_t g_task_ctx[XV6_MAX_TASK_CTX];
static SemaphoreHandle_t g_ctx_lock;

static void copy_cstr(char *dst, int dst_len, const char *src)
{
  if(dst == 0 || dst_len <= 0)
    return;
  if(src == 0)
    src = "";
  strncpy(dst, src, (size_t)dst_len - 1u);
  dst[dst_len - 1] = 0;
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

static xv6_task_ctx_t *task_ctx_get_internal(int create)
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

int vfs_task_ctx_init(void)
{
  memset(g_task_ctx, 0, sizeof(g_task_ctx));
  return 0;
}

xv6_task_ctx_t *vfs_task_ctx_get(void)
{
  return task_ctx_get_internal(1);
}

void vfs_task_ctx_set_errno(int err)
{
  xv6_task_ctx_t *ctx;
  if(err <= 0)
    err = EIO;
  ctx = task_ctx_get_internal(1);
  if(ctx)
    ctx->last_errno = err;
}

void vfs_task_ctx_clear_errno(void)
{
  xv6_task_ctx_t *ctx = task_ctx_get_internal(1);
  if(ctx)
    ctx->last_errno = 0;
}

int vfs_task_ctx_get_errno(void)
{
  xv6_task_ctx_t *ctx = task_ctx_get_internal(0);
  if(ctx && ctx->last_errno > 0)
    return ctx->last_errno;
  return EIO;
}

int vfs_task_ctx_set_stdio(int in_fd, int out_fd, int err_fd)
{
  xv6_task_ctx_t *ctx = task_ctx_get_internal(1);
  if(ctx == 0)
    return -1;
  ctx->stdio_active = 1;
  ctx->in_fd = in_fd;
  ctx->out_fd = out_fd;
  ctx->err_fd = err_fd;
  return 0;
}

void vfs_task_ctx_reset_stdio(void)
{
  xv6_task_ctx_t *ctx = task_ctx_get_internal(1);
  if(ctx){
    ctx->stdio_active = 0;
    ctx->in_fd = 0;
    ctx->out_fd = 1;
    ctx->err_fd = 2;
  }
}

int vfs_task_ctx_is_default_out(void)
{
  xv6_task_ctx_t *ctx = task_ctx_get_internal(0);
  if(ctx == 0 || !ctx->stdio_active)
    return 1;
  return (ctx->out_fd == 1) ? 1 : 0;
}

void vfs_task_ctx_cleanup(void)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  int i;

  task_ctx_lock();
  for(i = 0; i < XV6_MAX_TASK_CTX; i++){
    if(g_task_ctx[i].task == self){
      g_task_ctx[i].task = 0;
      memset(&g_task_ctx[i].ctx, 0, sizeof(g_task_ctx[i].ctx));
      break;
    }
  }
  task_ctx_unlock();
}

void vfs_task_ctx_cleanup_for_handle(void *task_handle)
{
  TaskHandle_t task = (TaskHandle_t)task_handle;
  int i;

  if(task == 0)
    return;

  task_ctx_lock();
  for(i = 0; i < XV6_MAX_TASK_CTX; i++){
    if(g_task_ctx[i].task == task){
      g_task_ctx[i].task = 0;
      memset(&g_task_ctx[i].ctx, 0, sizeof(g_task_ctx[i].ctx));
      break;
    }
  }
  task_ctx_unlock();
}

int vfs_task_ctx_getcwd(char *out, int len)
{
  xv6_task_ctx_t *ctx = task_ctx_get_internal(0);
  if(ctx == 0 || out == 0 || len <= 0)
    return -1;
  copy_cstr(out, len, ctx->cwd);
  return 0;
}

int vfs_task_ctx_chdir(const char *path)
{
  xv6_task_ctx_t *ctx = task_ctx_get_internal(1);
  if(ctx == 0 || path == 0)
    return -1;
  copy_cstr(ctx->cwd, sizeof(ctx->cwd), path);
  return 0;
}
