#include "runtime/shell_runtime.h"

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
#include <poll.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <time.h>
#include <sys/reent.h>
#include <signal.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "loader/elf_loader.h"
#include "platform/esp_flash_disk.h"
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
#define KSH_MAX_STAGES 8
#define KSH_MAX_ENV 16
#define KSH_ENV_KEY 24
#define KSH_ENV_VAL 128
#define KSH_BG_STACK 16384
#define KSH_MAX_CORES 8

_Static_assert(XV6_TASK_CTX_CAP >= (KSH_MAX_JOBS + 2), "XV6_TASK_CTX_CAP must cover shell + background jobs");
_Static_assert(XV6_PIPE_CAP >= (KSH_MAX_STAGES - 1), "XV6_PIPE_CAP must cover one full pipeline");

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
static int g_next_core_hint = 0;
static uint32 g_ulimit_ms = 0;
static int g_ulimit_heap_kb = 0;
static volatile int g_runtime_started = 0;
static SemaphoreHandle_t g_jobs_lock;
static SemaphoreHandle_t g_loader_lock;
static ksh_env_t g_env[KSH_MAX_ENV];
static TaskHandle_t g_interactive_task;

static int dispatch_command(int argc, char **argv, int run_bg);
static int eval_line_inner(const char *line, int *exit_code);
static int k_dup2(int oldfd, int newfd);


#include "shell_runtime_hostabi.inc"
#include "shell_runtime_shell.inc"
