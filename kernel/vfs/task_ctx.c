/**
 * @file task_ctx.c
 * @brief Task context implementation
 *
 * Manages per-task VFS state in a multi-threaded FreeRTOS environment.
 * Each FreeRTOS task that uses VFS operations needs a context to store:
 * - Standard I/O file descriptors (stdin, stdout, stderr)
 * - Current working directory
 * - Last errno value for error reporting
 *
 * The context is lazily allocated when a task first performs VFS operations.
 * This allows the VFS to work with multiple concurrent tasks.
 */
#include "vfs/task_ctx.h"

#include <errno.h>
#include <string.h>

#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "loader/elf_loader.h"
#include "vfs/vfs.h"

#define XV6_MAX_TASK_CTX XV6_TASK_CTX_CAP

/**
 * @brief Task context slot
 *
 * Maps a FreeRTOS task handle to its VFS context.
 */
typedef struct {
  TaskHandle_t task;    /**< FreeRTOS task handle, 0 if free */
  xv6_task_ctx_t ctx;  /**< VFS context for this task */
} xv6_task_ctx_slot_t;

/** Global task context table */
static xv6_task_ctx_slot_t g_task_ctx[XV6_MAX_TASK_CTX];

/** Mutex for thread-safe context access */
static SemaphoreHandle_t g_ctx_lock;

/**
 * @brief Copy null-terminated string with bounds checking
 *
 * @param dst     Destination buffer
 * @param dst_len Destination buffer size
 * @param src     Source string
 */
static void copy_cstr(char *dst, int dst_len, const char *src)
{
  if(dst == 0 || dst_len <= 0)
    return;
  if(src == 0)
    src = "";
  strncpy(dst, src, (size_t)dst_len - 1u);
  dst[dst_len - 1] = 0;
}

/**
 * @brief Acquire task context lock
 *
 * Creates the mutex if needed, then takes it.
 */
static void task_ctx_lock(void)
{
  if(g_ctx_lock == 0)
    g_ctx_lock = xSemaphoreCreateMutex();
  if(g_ctx_lock)
    (void)xSemaphoreTake(g_ctx_lock, portMAX_DELAY);
}

/**
 * @brief Release task context lock
 */
static void task_ctx_unlock(void)
{
  if(g_ctx_lock)
    (void)xSemaphoreGive(g_ctx_lock);
}

/**
 * @brief Internal function to get or create task context
 *
 * @param create If 1, create context if not found; if 0, return NULL
 * @return Pointer to task context, or NULL
 */
static xv6_task_ctx_t *task_ctx_get_internal(int create)
{
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  int i;
  int free_slot = -1;

  task_ctx_lock();

  /* Search for existing context or find free slot */
  for(i = 0; i < XV6_MAX_TASK_CTX; i++){
    if(g_task_ctx[i].task == self){
      /* Found existing context for this task */
      task_ctx_unlock();
      return &g_task_ctx[i].ctx;
    }
    /* Track first free slot */
    if(g_task_ctx[i].task == 0 && free_slot < 0)
      free_slot = i;
  }

  /* Create new context if requested and slot available */
  if(create && free_slot >= 0){
    g_task_ctx[free_slot].task = self;
    memset(&g_task_ctx[free_slot].ctx, 0, sizeof(g_task_ctx[free_slot].ctx));
    
    /* Initialize default stdio FDs */
    g_task_ctx[free_slot].ctx.in_fd = 0;
    g_task_ctx[free_slot].ctx.out_fd = 1;
    g_task_ctx[free_slot].ctx.err_fd = 2;
    
    /* Default working directory is root */
    copy_cstr(g_task_ctx[free_slot].ctx.cwd, sizeof(g_task_ctx[free_slot].ctx.cwd), "/");
    
    task_ctx_unlock();
    return &g_task_ctx[free_slot].ctx;
  }

  task_ctx_unlock();
  return 0;
}

int vfs_task_ctx_init(void)
{
  /* Clear all task context slots */
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
  
  /* Normalize error value */
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
  
  /* Find and clear the current task's context */
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

  /* Handle NULL gracefully */
  if(task == 0)
    return;

  task_ctx_lock();
  
  /* Find and clear the specified task's context */
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
