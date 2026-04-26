#include "runtime/shell_runtime.h"
#include "runtime/shell_parse.h"

#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <time.h>
#include "lwip/api.h"
#include <sys/reent.h>
#include <signal.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "soc/soc_caps.h"
#if SOC_WIFI_SUPPORTED
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#endif
#include "loader/elf_loader.h"
#include "platform/esp_flash_disk.h"
#include "platform/xv6_esp_boot.h"
#include "esp_memory_utils.h"
#include "platform/hal.h"
#include "hostabi/hostabi_dirent.h"
#include "hostabi/hostabi_exports.h"
#include "hostabi/hostabi_posix_fs.h"
#include "hostabi/hostabi_posix_io.h"
#include "hostabi/hostabi_pty.h"
#include "modules/module_manager.h"
#include "core/param.h"
#include "vfs/vfs.h"

#define KSH_MAX_JOBS 32
#define KSH_MAX_ARGS 32
/* Verified safe concurrent applet pipeline depth. Longer pipelines fail fast. */
#define KSH_MAX_STAGES 3
#define KSH_MAX_ENV 16
#define KSH_ENV_KEY 24
#define KSH_ENV_VAL 128
#define KSH_BG_STACK 12288
#define KSH_MAX_CORES 8

_Static_assert(XV6_TASK_CTX_CAP >= (KSH_MAX_JOBS + 2), "XV6_TASK_CTX_CAP must cover shell + background jobs");
_Static_assert(XV6_PIPE_CAP >= (KSH_MAX_STAGES - 1), "XV6_PIPE_CAP must cover one full pipeline");

enum {
  JOB_REASON_NONE = 0,
  JOB_REASON_EXIT,
  JOB_REASON_TIMEOUT,
  JOB_REASON_KILLED,
  JOB_REASON_STOPPED,
};

typedef struct {
  int used;
  int id;
  int pid;
  int ppid;
  int pgid;
  int sid;
  int done;
  int stopped;
  int wait_pending_exit;
  int wait_pending_stop;
  int wait_pending_cont;
  int exit_code;
  int reason;
  int stop_signal;
  int user_visible;
  int is_pipe;
  int max_heap_kb;
  uint32 max_runtime_ms;
  uint32 started_ms;
  TaskHandle_t task;
  void *task_ctx;
  int assigned_core;
  int running_core;
  char cmd[96];
} ksh_job_t;

typedef struct {
  int slot;
  int pid;
  int ppid;
  int pgid;
  int sid;
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

static ksh_job_t *g_jobs;
/* Reserve pid/pgid=1 for interactive shell process metadata. */
static int g_next_job_id = 2;
static int g_next_core_hint = 0;
static uint32 g_ulimit_ms = 0;
static int g_ulimit_heap_kb = 0;
static volatile int g_runtime_started = 0;
static SemaphoreHandle_t g_jobs_lock;
static ksh_env_t g_env[KSH_MAX_ENV];
static TaskHandle_t g_interactive_task;

static int dispatch_command(int argc, char **argv, int run_bg);
static int eval_line_inner(const char *line, int *exit_code);
static int k_dup2(int oldfd, int newfd);

static void *ksh_alloc_data(size_t sz)
{
  void *p = 0;
#ifdef MALLOC_CAP_SPIRAM
  p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  if(p == 0)
    p = heap_caps_malloc(sz, MALLOC_CAP_8BIT);
  return p;
}

static int ksh_jobs_ensure(void)
{
  if(g_jobs)
    return 0;
  g_jobs = (ksh_job_t *)ksh_alloc_data((size_t)KSH_MAX_JOBS * sizeof(*g_jobs));
  if(g_jobs == 0)
    return -1;
  memset(g_jobs, 0, (size_t)KSH_MAX_JOBS * sizeof(*g_jobs));
  return 0;
}

static void ksh_jobs_release(void)
{
  if(g_jobs){
    heap_caps_free(g_jobs);
    g_jobs = 0;
  }
}

#include "shell_runtime_hostabi.inc"
#include "shell_runtime_shell.inc"
