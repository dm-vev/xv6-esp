#include "ksh.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "elf_loader.h"
#include "esp_flash_disk.h"
#include "hal.h"
#include "param.h"
#include "xv6fs_ro.h"

#define KSH_MAX_JOBS 32
#define KSH_MAX_ARGS 32
#define KSH_MAX_STAGES 8
#define KSH_MAX_ENV 16
#define KSH_ENV_KEY 24
#define KSH_ENV_VAL 128
#define KSH_BG_STACK 8192

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
  int is_pipe;
  int max_heap_kb;
  uint32 max_runtime_ms;
  uint32 started_ms;
  TaskHandle_t task;
  void *task_ctx;
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
static uint32 g_ulimit_ms = 0;
static int g_ulimit_heap_kb = 0;
static SemaphoreHandle_t g_jobs_lock;
static SemaphoreHandle_t g_loader_lock;
static ksh_env_t g_env[KSH_MAX_ENV];

static int dispatch_command(int argc, char **argv, int run_bg);

static int k_ticks(void)
{
  return (int)hal_ticks();
}

static int k_free_heap(void)
{
  return (int)hal_free_heap_bytes();
}

static int k_puts(const char *s)
{
  if(s == 0)
    return -1;
  while(*s)
    hal_console_putc(*s++);
  hal_console_putc('\r');
  hal_console_putc('\n');
  return 0;
}

static int k_fs_readdir_path(const char *path, int index, char *name_out, int name_out_len, uint16 *type_out,
                             uint32 *size_out)
{
  return xv6fs_list_path(path, index, name_out, name_out_len, type_out, size_out);
}

static void tty_putc(int c)
{
  hal_console_putc(c);
}

static void tty_puts(const char *s)
{
  while(*s)
    tty_putc(*s++);
}

static void putc_console(int c)
{
  char ch = (char)c;
  if(xv6_write(1, &ch, 1) != 1)
    tty_putc(c);
}

static void puts_console(const char *s)
{
  while(*s)
    putc_console(*s++);
}

static void eputc_console(int c)
{
  char ch = (char)c;
  if(xv6_write(2, &ch, 1) != 1)
    hal_console_putc(c);
}

static void eputs_console(const char *s)
{
  while(*s)
    eputc_console(*s++);
}

static void puts_line(const char *s)
{
  puts_console(s);
  puts_console("\r\n");
}

static void eputs_line(const char *s)
{
  eputs_console(s);
  eputs_console("\r\n");
}

static void k_vprintf_fd(int fd, const char *fmt, va_list ap)
{
  char buf[256];
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  if(n < 0)
    return;
  if(n >= (int)sizeof(buf))
    n = (int)sizeof(buf) - 1;
  if(fd == 2){
    if(xv6_write(2, buf, (uint32)n) != n)
      return;
  } else {
    if(xv6_write(1, buf, (uint32)n) != n)
      return;
  }
}

static void k_printf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  k_vprintf_fd(1, fmt, ap);
  va_end(ap);
}

static void k_eprintf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  k_vprintf_fd(2, fmt, ap);
  va_end(ap);
}

static void print_u32(uint32 v)
{
  char tmp[11];
  int i = 0;

  if(v == 0){
    putc_console('0');
    return;
  }

  while(v > 0 && i < (int)(sizeof(tmp) - 1)){
    tmp[i++] = (char)('0' + (v % 10));
    v /= 10;
  }
  while(i > 0)
    putc_console(tmp[--i]);
}

static int parse_u32_dec(const char *s, uint32 *out)
{
  uint32 v = 0;
  if(s == 0 || *s == 0 || out == 0)
    return -1;
  while(*s){
    if(*s < '0' || *s > '9')
      return -1;
    v = v * 10 + (uint32)(*s - '0');
    s++;
  }
  *out = v;
  return 0;
}

static int env_find_slot(const char *key)
{
  int i;
  for(i = 0; i < KSH_MAX_ENV; i++){
    if(g_env[i].used && strcmp(g_env[i].key, key) == 0)
      return i;
  }
  return -1;
}

static const char *env_get(const char *key)
{
  int i = env_find_slot(key);
  if(i >= 0)
    return g_env[i].val;
  return 0;
}

static int env_set(const char *key, const char *val)
{
  int i = env_find_slot(key);
  int free_i = -1;
  int j;

  if(key == 0 || val == 0 || key[0] == 0 || strlen(key) >= KSH_ENV_KEY || strlen(val) >= KSH_ENV_VAL)
    return -1;
  for(j = 0; key[j]; j++){
    if(!(key[j] == '_' || (key[j] >= '0' && key[j] <= '9') || (key[j] >= 'A' && key[j] <= 'Z') ||
         (key[j] >= 'a' && key[j] <= 'z')))
      return -1;
  }

  if(i < 0){
    for(j = 0; j < KSH_MAX_ENV; j++){
      if(!g_env[j].used){
        free_i = j;
        break;
      }
    }
    if(free_i < 0)
      return -1;
    i = free_i;
    memset(&g_env[i], 0, sizeof(g_env[i]));
    g_env[i].used = 1;
    strcpy(g_env[i].key, key);
  }
  strcpy(g_env[i].val, val);
  return 0;
}

static void env_unset(const char *key)
{
  int i = env_find_slot(key);
  if(i >= 0)
    memset(&g_env[i], 0, sizeof(g_env[i]));
}

static void env_sync_pwd(void)
{
  char cwd[MAXPATH];
  if(xv6_getcwd(cwd, sizeof(cwd)) == 0)
    (void)env_set("PWD", cwd);
}

static void env_init_defaults(void)
{
  memset(g_env, 0, sizeof(g_env));
  (void)env_set("PATH", "/bin:/usr/bin:.");
  (void)env_set("HOME", "/");
  env_sync_pwd();
}

static const char *job_reason_str(int reason)
{
  switch(reason){
  case JOB_REASON_EXIT:
    return "exit";
  case JOB_REASON_TIMEOUT:
    return "timeout";
  case JOB_REASON_KILLED:
    return "killed";
  default:
    return "-";
  }
}

static int parse_line(char *line, char **argv, int max_args)
{
  char *src = line;
  char *dst = line;
  char *tok = 0;
  int argc = 0;
  int in_sq = 0;
  int in_dq = 0;
  int esc = 0;

  while(*src){
    char ch = *src++;

    if(esc){
      if(tok == 0)
        tok = dst;
      *dst++ = ch;
      esc = 0;
      continue;
    }

    if(ch == '\\' && !in_sq){
      esc = 1;
      continue;
    }

    if(in_sq){
      if(ch == '\'')
        in_sq = 0;
      else {
        if(tok == 0)
          tok = dst;
        *dst++ = ch;
      }
      continue;
    }

    if(in_dq){
      if(ch == '"')
        in_dq = 0;
      else {
        if(tok == 0)
          tok = dst;
        *dst++ = ch;
      }
      continue;
    }

    if(ch == '\''){
      if(tok == 0)
        tok = dst;
      in_sq = 1;
      continue;
    }

    if(ch == '"'){
      if(tok == 0)
        tok = dst;
      in_dq = 1;
      continue;
    }

    if(ch == ' ' || ch == '\t'){
      if(tok){
        *dst++ = 0;
        if(argc >= max_args)
          return max_args;
        argv[argc++] = tok;
        tok = 0;
      }
      continue;
    }

    if(ch == '|' || ch == '&' || ch == '<' || ch == '>'){
      if(tok){
        *dst++ = 0;
        if(argc >= max_args)
          return max_args;
        argv[argc++] = tok;
        tok = 0;
      }
      if(argc >= max_args)
        return max_args;
      if(ch == '|')
        argv[argc++] = "|";
      else if(ch == '&')
        argv[argc++] = "&";
      else if(ch == '<')
        argv[argc++] = "<";
      else if(*src == '>'){
        src++;
        argv[argc++] = ">>";
      } else
        argv[argc++] = ">";
      continue;
    }

    if(tok == 0)
      tok = dst;
    *dst++ = ch;
  }

  if(esc){
    if(tok == 0)
      tok = dst;
    *dst++ = '\\';
  }

  if(in_sq || in_dq)
    return -1;

  if(tok){
    *dst++ = 0;
    if(argc < max_args)
      argv[argc++] = tok;
  }

  return argc;
}

static void cmd_help(void)
{
  puts_line("commands:");
  puts_line("  help");
  puts_line("  <elf-command> [args]");
  puts_line("  <elf-command> [args] &");
  puts_line("  <a> | <b> | <c> ...");
  puts_line("  redirection: < > >> 2> 2>>");
  puts_line("  cd [dir], pwd");
  puts_line("  env, export NAME=VALUE, unset NAME");
  puts_line("  ps");
  puts_line("  jobs");
  puts_line("  fg <jobid>");
  puts_line("  kill <jobid>");
  puts_line("  wait [jobid]");
  puts_line("  time <cmd...>");
  puts_line("  ulimit [-t ms] [-m kb]");
  puts_line("  limit <ms> <heap_kb> <cmd...> [&]");
  puts_line("  reboot");
}

static int try_read_exec_image(const char *cmd, void **out_image, uint32 *out_size, char *resolved, int resolved_len)
{
  char pathbuf[MAXPATH];
  const char *path_env;
  const char *p;

  if(cmd == 0 || out_image == 0 || out_size == 0)
    return -1;
  *out_image = 0;
  *out_size = 0;
  if(resolved && resolved_len > 0)
    resolved[0] = 0;

  if(strchr(cmd, '/')){
    if(xv6fs_read_file_alloc_path(cmd, out_image, out_size) == 0){
      if(resolved && resolved_len > 0){
        strncpy(resolved, cmd, resolved_len - 1);
        resolved[resolved_len - 1] = 0;
      }
      return 0;
    }
    if(snprintf(pathbuf, sizeof(pathbuf), "%s.elf", cmd) > 0 && xv6fs_read_file_alloc_path(pathbuf, out_image, out_size) == 0){
      if(resolved && resolved_len > 0){
        strncpy(resolved, pathbuf, resolved_len - 1);
        resolved[resolved_len - 1] = 0;
      }
      return 0;
    }
    return -1;
  }

  path_env = env_get("PATH");
  if(path_env == 0 || path_env[0] == 0)
    return -1;

  p = path_env;
  while(1){
    const char *seg = p;
    int seg_len = 0;
    while(*p && *p != ':'){
      p++;
      seg_len++;
    }

    if(seg_len == 0){
      if(snprintf(pathbuf, sizeof(pathbuf), "%s", cmd) > 0 && xv6fs_read_file_alloc_path(pathbuf, out_image, out_size) == 0){
        if(resolved && resolved_len > 0){
          strncpy(resolved, pathbuf, resolved_len - 1);
          resolved[resolved_len - 1] = 0;
        }
        return 0;
      }
      if(snprintf(pathbuf, sizeof(pathbuf), "%s.elf", cmd) > 0 && xv6fs_read_file_alloc_path(pathbuf, out_image, out_size) == 0){
        if(resolved && resolved_len > 0){
          strncpy(resolved, pathbuf, resolved_len - 1);
          resolved[resolved_len - 1] = 0;
        }
        return 0;
      }
    } else {
      if(seg_len >= (int)sizeof(pathbuf))
        seg_len = (int)sizeof(pathbuf) - 1;
      memcpy(pathbuf, seg, (unsigned)seg_len);
      pathbuf[seg_len] = 0;
      if(snprintf(pathbuf + seg_len, sizeof(pathbuf) - (unsigned)seg_len, "/%s", cmd) > 0 &&
         xv6fs_read_file_alloc_path(pathbuf, out_image, out_size) == 0){
        if(resolved && resolved_len > 0){
          strncpy(resolved, pathbuf, resolved_len - 1);
          resolved[resolved_len - 1] = 0;
        }
        return 0;
      }
      if(snprintf(pathbuf + seg_len, sizeof(pathbuf) - (unsigned)seg_len, "/%s.elf", cmd) > 0 &&
         xv6fs_read_file_alloc_path(pathbuf, out_image, out_size) == 0){
        if(resolved && resolved_len > 0){
          strncpy(resolved, pathbuf, resolved_len - 1);
          resolved[resolved_len - 1] = 0;
        }
        return 0;
      }
    }

    if(*p == 0)
      break;
    p++;
  }

  return -1;
}

static int run_elf_command(int argc, char **argv, int *exit_code, int in_fd, int out_fd, int err_fd, int max_heap_kb)
{
  elf_module_t *m;
  void *image = 0;
  uint32 image_size = 0;
  int retv = 0;
  char module_name[MAXPATH];

  if(argc <= 0 || argv == 0 || argv[0] == 0 || argv[0][0] == 0)
    return -1;

  if(exit_code)
    *exit_code = 127;

  if(max_heap_kb > 0){
    int free_kb = k_free_heap() / 1024;
    if(free_kb < max_heap_kb){
      puts_line("exec: blocked by memory limit");
      if(exit_code)
        *exit_code = 125;
      return -1;
    }
  }

  xv6_stdio_set_fds(in_fd, out_fd, err_fd);

  if(g_loader_lock)
    (void)xSemaphoreTake(g_loader_lock, portMAX_DELAY);
  strncpy(module_name, argv[0], sizeof(module_name) - 1);
  module_name[sizeof(module_name) - 1] = 0;
  m = elf_module_find(module_name);
  if(m == 0){
    if(try_read_exec_image(argv[0], &image, &image_size, module_name, sizeof(module_name)) != 0 || image == 0){
      if(g_loader_lock)
        (void)xSemaphoreGive(g_loader_lock);
      xv6_stdio_reset_fds();
      puts_line("exec: command not found");
      return -1;
    }
    if(elf_module_load_from_bytes(module_name, image, image_size, &m) != 0){
      free(image);
      if(g_loader_lock)
        (void)xSemaphoreGive(g_loader_lock);
      xv6_stdio_reset_fds();
      puts_line("exec: elf load failed");
      return -1;
    }
    free(image);
  }
  if(g_loader_lock)
    (void)xSemaphoreGive(g_loader_lock);

  if(elf_module_call_main(m, argc, argv, &retv) != 0){
    xv6_stdio_reset_fds();
    puts_line("exec: entry call failed");
    if(exit_code)
      *exit_code = 126;
    return -1;
  }

  xv6_stdio_reset_fds();

  if(exit_code)
    *exit_code = retv;
  if(retv != 0){
    puts_console(argv[0]);
    puts_console(": exit=");
    print_u32((uint32)retv);
    puts_line("");
  }
  return 0;
}

static int job_find_slot_by_id(int id)
{
  int i;
  for(i = 0; i < KSH_MAX_JOBS; i++){
    if(g_jobs[i].used && g_jobs[i].id == id)
      return i;
  }
  return -1;
}

static void close_job_fd_if_needed(int fd)
{
  if(fd >= 3)
    (void)xv6_close(fd);
}

static void free_job_ctx(ksh_job_task_t *t)
{
  int i;
  if(t == 0)
    return;
  if(t->in_fd >= 3)
    close_job_fd_if_needed(t->in_fd);
  if(t->out_fd >= 3 && t->out_fd != t->in_fd)
    close_job_fd_if_needed(t->out_fd);
  if(t->err_fd >= 3 && t->err_fd != t->in_fd && t->err_fd != t->out_fd)
    close_job_fd_if_needed(t->err_fd);
  for(i = 0; i < t->argc; i++)
    free(t->argv[i]);
  free(t->argv);
  free(t);
}

static void enforce_job_limits(void)
{
  int i;
  uint32 now = (uint32)k_ticks();
  TaskHandle_t kill_tasks[KSH_MAX_JOBS];
  ksh_job_task_t *kill_ctx[KSH_MAX_JOBS];
  int nkill = 0;

  memset(kill_tasks, 0, sizeof(kill_tasks));
  memset(kill_ctx, 0, sizeof(kill_ctx));

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  for(i = 0; i < KSH_MAX_JOBS; i++){
    if(!g_jobs[i].used || g_jobs[i].done)
      continue;
    if(g_jobs[i].max_runtime_ms > 0 && ((uint32)(now - g_jobs[i].started_ms) * 10u) > g_jobs[i].max_runtime_ms){
      if(nkill < KSH_MAX_JOBS){
        kill_tasks[nkill] = g_jobs[i].task;
        kill_ctx[nkill] = (ksh_job_task_t *)g_jobs[i].task_ctx;
        nkill++;
      }
      g_jobs[i].done = 1;
      g_jobs[i].exit_code = 124;
      g_jobs[i].reason = JOB_REASON_TIMEOUT;
      g_jobs[i].task = 0;
      g_jobs[i].task_ctx = 0;
    }
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  for(i = 0; i < nkill; i++){
    if(kill_tasks[i])
      vTaskDelete(kill_tasks[i]);
    if(kill_ctx[i])
      free_job_ctx(kill_ctx[i]);
  }
}

static void job_task(void *arg)
{
  ksh_job_task_t *t = (ksh_job_task_t *)arg;
  int exit_code = 127;

  if(t->cwd[0])
    (void)xv6_chdir(t->cwd);
  (void)run_elf_command(t->argc, t->argv, &exit_code, t->in_fd, t->out_fd, t->err_fd, t->max_heap_kb);

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  if(t->slot >= 0 && t->slot < KSH_MAX_JOBS && g_jobs[t->slot].used){
    g_jobs[t->slot].done = 1;
    g_jobs[t->slot].exit_code = exit_code;
    g_jobs[t->slot].reason = (g_jobs[t->slot].reason == JOB_REASON_NONE) ? JOB_REASON_EXIT : g_jobs[t->slot].reason;
    g_jobs[t->slot].task = 0;
    g_jobs[t->slot].task_ctx = 0;
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  free_job_ctx(t);
  xv6_task_ctx_cleanup();
  vTaskDelete(NULL);
}

static int terminate_job_id(int id, int exit_code, int reason)
{
  int slot;
  TaskHandle_t h = 0;
  ksh_job_task_t *ctx = 0;

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  slot = job_find_slot_by_id(id);
  if(slot >= 0 && g_jobs[slot].used && !g_jobs[slot].done){
    h = g_jobs[slot].task;
    ctx = (ksh_job_task_t *)g_jobs[slot].task_ctx;
    g_jobs[slot].done = 1;
    g_jobs[slot].exit_code = exit_code;
    g_jobs[slot].reason = reason;
    g_jobs[slot].task = 0;
    g_jobs[slot].task_ctx = 0;
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(slot < 0)
    return -1;
  if(h)
    vTaskDelete(h);
  if(ctx)
    free_job_ctx(ctx);
  return 0;
}

static int wait_job_id_ex(int id, int *out_exit_code, int consume, int allow_ctrl_c)
{
  while(1){
    int slot;
    int done = 0;
    int exit_code = 0;

    if(g_jobs_lock)
      (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
    slot = job_find_slot_by_id(id);
    if(slot < 0){
      if(g_jobs_lock)
        (void)xSemaphoreGive(g_jobs_lock);
      return -1;
    }

    if(g_jobs[slot].done){
      done = 1;
      exit_code = g_jobs[slot].exit_code;
      if(consume)
        memset(&g_jobs[slot], 0, sizeof(g_jobs[slot]));
    }
    if(g_jobs_lock)
      (void)xSemaphoreGive(g_jobs_lock);

    if(done){
      if(out_exit_code)
        *out_exit_code = exit_code;
      return 0;
    }

    if(allow_ctrl_c){
      int c = hal_console_getc();
      if(c == 0x03){
        if(terminate_job_id(id, 130, JOB_REASON_KILLED) == 0){
          puts_line("^C");
          if(out_exit_code)
            *out_exit_code = 130;
          if(consume){
            if(g_jobs_lock)
              (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
            slot = job_find_slot_by_id(id);
            if(slot >= 0)
              memset(&g_jobs[slot], 0, sizeof(g_jobs[slot]));
            if(g_jobs_lock)
              (void)xSemaphoreGive(g_jobs_lock);
          }
          return 0;
        }
      }
    }

    enforce_job_limits();
    hal_delay_ms(10);
  }
}

static int wait_job_id(int id, int *out_exit_code, int consume)
{
  return wait_job_id_ex(id, out_exit_code, consume, 0);
}

static int spawn_background_ex(int argc, char **argv, int in_fd, int out_fd, int err_fd, int is_pipe, int quiet_start,
                               int max_heap_kb, uint32 max_runtime_ms, int *out_job_id)
{
  int i, j;
  int slot = -1;
  int id = 0;
  int pos = 0;
  TaskHandle_t handle = 0;
  ksh_job_task_t *t = 0;

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  for(i = 0; i < KSH_MAX_JOBS; i++){
    if(!g_jobs[i].used){
      slot = i;
      break;
    }
  }
  if(slot < 0){
    if(g_jobs_lock)
      (void)xSemaphoreGive(g_jobs_lock);
    puts_line("jobs: table full");
    return -1;
  }

  t = (ksh_job_task_t *)calloc(1, sizeof(*t));
  if(t == 0){
    if(g_jobs_lock)
      (void)xSemaphoreGive(g_jobs_lock);
    puts_line("jobs: no memory");
    return -1;
  }

  t->argv = (char **)calloc((unsigned)argc + 1, sizeof(char *));
  if(t->argv == 0){
    if(g_jobs_lock)
      (void)xSemaphoreGive(g_jobs_lock);
    free(t);
    puts_line("jobs: no memory");
    return -1;
  }

  t->slot = slot;
  t->argc = argc;
  t->in_fd = in_fd;
  t->out_fd = out_fd;
  t->err_fd = err_fd;
  t->max_heap_kb = max_heap_kb;
  if(xv6_getcwd(t->cwd, sizeof(t->cwd)) != 0)
    strcpy(t->cwd, "/");

  if(t->in_fd >= 3){
    t->in_fd = xv6_dup(t->in_fd);
    if(t->in_fd < 0){
      free(t->argv);
      free(t);
      if(g_jobs_lock)
        (void)xSemaphoreGive(g_jobs_lock);
      puts_line("jobs: fd dup failed");
      return -1;
    }
  }
  if(t->out_fd >= 3){
    int dupfd = xv6_dup(t->out_fd);
    if(dupfd < 0){
      free_job_ctx(t);
      if(g_jobs_lock)
        (void)xSemaphoreGive(g_jobs_lock);
      puts_line("jobs: fd dup failed");
      return -1;
    }
    t->out_fd = dupfd;
  }
  if(t->err_fd >= 3){
    int dupfd = xv6_dup(t->err_fd);
    if(dupfd < 0){
      free_job_ctx(t);
      if(g_jobs_lock)
        (void)xSemaphoreGive(g_jobs_lock);
      puts_line("jobs: fd dup failed");
      return -1;
    }
    t->err_fd = dupfd;
  }

  for(i = 0; i < argc; i++){
    size_t n = strlen(argv[i]) + 1;
    t->argv[i] = (char *)malloc(n);
    if(t->argv[i] == 0){
      for(j = 0; j < i; j++)
        free(t->argv[j]);
      free(t->argv);
      free(t);
      if(g_jobs_lock)
        (void)xSemaphoreGive(g_jobs_lock);
      puts_line("jobs: no memory");
      return -1;
    }
    memcpy(t->argv[i], argv[i], n);
  }
  t->argv[argc] = 0;

  id = g_next_job_id++;
  if(g_next_job_id < 1)
    g_next_job_id = 1;

  memset(&g_jobs[slot], 0, sizeof(g_jobs[slot]));
  g_jobs[slot].used = 1;
  g_jobs[slot].id = id;
  g_jobs[slot].is_pipe = is_pipe;
  g_jobs[slot].max_heap_kb = max_heap_kb;
  g_jobs[slot].max_runtime_ms = max_runtime_ms;
  g_jobs[slot].started_ms = (uint32)k_ticks();
  g_jobs[slot].reason = JOB_REASON_NONE;
  g_jobs[slot].task_ctx = t;

  for(i = 0; i < argc; i++){
    int n = snprintf(g_jobs[slot].cmd + pos, sizeof(g_jobs[slot].cmd) - (unsigned)pos, "%s%s", (i ? " " : ""),
                     argv[i]);
    if(n <= 0 || pos + n >= (int)sizeof(g_jobs[slot].cmd)){
      g_jobs[slot].cmd[sizeof(g_jobs[slot].cmd) - 1] = 0;
      break;
    }
    pos += n;
  }

  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(xTaskCreate(job_task, "xv6_bg", KSH_BG_STACK, t, 5, &handle) != pdPASS){
    if(g_jobs_lock)
      (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
    memset(&g_jobs[slot], 0, sizeof(g_jobs[slot]));
    if(g_jobs_lock)
      (void)xSemaphoreGive(g_jobs_lock);
    free_job_ctx(t);
    puts_line("jobs: spawn failed");
    return -1;
  }

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  if(g_jobs[slot].used)
    g_jobs[slot].task = handle;
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(!quiet_start){
    putc_console('[');
    print_u32((uint32)id);
    puts_line("] started");
  }

  if(out_job_id)
    *out_job_id = id;
  return 0;
}

static int run_foreground_with_limits(int argc, char **argv, int in_fd, int out_fd, int err_fd, int max_heap_kb,
                                      uint32 max_runtime_ms)
{
  int rc;
  int job_id = -1;
  int exit_code = 127;

  rc = spawn_background_ex(argc, argv, in_fd, out_fd, err_fd, 0, 1, max_heap_kb, max_runtime_ms, &job_id);
  if(rc != 0)
    return -1;
  rc = wait_job_id_ex(job_id, &exit_code, 1, 1);
  if(rc != 0)
    return -1;
  if(exit_code == 124){
    puts_line("limit: timeout");
    return -1;
  }
  return exit_code == 0 ? 0 : -1;
}

static int is_fd_token(const char *s, int *out_fd)
{
  if(s && s[0] >= '0' && s[0] <= '2' && s[1] == 0){
    if(out_fd)
      *out_fd = (int)(s[0] - '0');
    return 1;
  }
  return 0;
}

static int is_redir_token(const char *s)
{
  return (strcmp(s, "<") == 0 || strcmp(s, ">") == 0 || strcmp(s, ">>") == 0);
}

static int is_control_token(const char *s)
{
  return (strcmp(s, "|") == 0 || strcmp(s, "&") == 0 || is_redir_token(s));
}

static void close_io_custom_fds(const ksh_io_t *io, const ksh_io_t *base)
{
  int base_fds[3];
  int vals[3];
  int i, j;

  if(io == 0 || base == 0)
    return;

  base_fds[0] = base->in_fd;
  base_fds[1] = base->out_fd;
  base_fds[2] = base->err_fd;
  vals[0] = io->in_fd;
  vals[1] = io->out_fd;
  vals[2] = io->err_fd;

  for(i = 0; i < 3; i++){
    int is_base = 0;
    if(vals[i] < 3)
      continue;
    for(j = 0; j < 3; j++){
      if(vals[i] == base_fds[j]){
        is_base = 1;
        break;
      }
    }
    if(is_base)
      continue;
    for(j = 0; j < i; j++){
      if(vals[i] == vals[j]){
        is_base = 1;
        break;
      }
    }
    if(!is_base)
      (void)xv6_close(vals[i]);
  }
}

static int parse_exec_and_redir(int argc, char **argv, const ksh_io_t *base_io, char **exec_argv, int max_exec,
                                int *out_argc, ksh_io_t *out_io)
{
  int i;
  int n = 0;
  ksh_io_t io;

  if(base_io == 0 || exec_argv == 0 || out_argc == 0 || out_io == 0)
    return -1;

  io = *base_io;
  for(i = 0; i < argc; i++){
    int target_fd;
    int flags;
    int fd;
    int *dst;
    const char *op = argv[i];
    const char *path;

    if(!is_redir_token(op)){
      if(strcmp(op, "|") == 0 || strcmp(op, "&") == 0){
        puts_line("syntax: bad token");
        goto fail;
      }
      if(n >= max_exec - 1){
        puts_line("exec: too many args");
        goto fail;
      }
      exec_argv[n++] = argv[i];
      continue;
    }

    target_fd = (op[0] == '<') ? 0 : 1;
    if(n > 0 && is_fd_token(exec_argv[n - 1], &target_fd))
      n--;
    if(op[0] == '<' && target_fd != 0){
      puts_line("redir: bad input fd");
      goto fail;
    }

    if(i + 1 >= argc){
      puts_line("redir: missing path");
      goto fail;
    }
    path = argv[++i];
    if(is_control_token(path)){
      puts_line("redir: bad path");
      goto fail;
    }

    if(op[0] == '<')
      flags = XV6_O_RDONLY;
    else if(strcmp(op, ">>") == 0)
      flags = XV6_O_WRONLY | XV6_O_CREAT | XV6_O_APPEND;
    else
      flags = XV6_O_WRONLY | XV6_O_CREAT | XV6_O_TRUNC;

    fd = xv6_open(path, flags);
    if(fd < 0){
      puts_console("redir: open failed: ");
      puts_line(path);
      goto fail;
    }

    if(target_fd == 0)
      dst = &io.in_fd;
    else if(target_fd == 1)
      dst = &io.out_fd;
    else if(target_fd == 2)
      dst = &io.err_fd;
    else {
      xv6_close(fd);
      puts_line("redir: bad fd");
      goto fail;
    }

    if(*dst >= 3 && *dst != base_io->in_fd && *dst != base_io->out_fd && *dst != base_io->err_fd)
      xv6_close(*dst);
    *dst = fd;
  }

  if(n <= 0){
    puts_line("syntax: empty command");
    goto fail;
  }
  exec_argv[n] = 0;
  *out_argc = n;
  *out_io = io;
  return 0;

fail:
  close_io_custom_fds(&io, base_io);
  return -1;
}

static int find_pipe_pos(int argc, char **argv)
{
  int i;
  for(i = 0; i < argc; i++){
    if(strcmp(argv[i], "|") == 0)
      return i;
  }
  return -1;
}

static int build_pipeline(int argc, char **argv, int *starts, int *lens, int max_stages)
{
  int i;
  int stage = 0;
  int start = 0;

  for(i = 0; i < argc; i++){
    if(strcmp(argv[i], "|") != 0)
      continue;
    if(i == start || stage >= max_stages)
      return -1;
    starts[stage] = start;
    lens[stage] = i - start;
    stage++;
    start = i + 1;
  }

  if(start >= argc || stage >= max_stages)
    return -1;
  starts[stage] = start;
  lens[stage] = argc - start;
  stage++;
  return stage;
}

static int run_pipeline(int argc, char **argv, int run_bg, int max_heap_kb, uint32 max_runtime_ms)
{
  int stage_starts[KSH_MAX_STAGES];
  int stage_lens[KSH_MAX_STAGES];
  int stage_count;
  int pipe_r[KSH_MAX_STAGES - 1];
  int pipe_w[KSH_MAX_STAGES - 1];
  int jobs[KSH_MAX_STAGES];
  int njobs = 0;
  int i;
  int final_rc = 0;

  for(i = 0; i < KSH_MAX_STAGES - 1; i++){
    pipe_r[i] = -1;
    pipe_w[i] = -1;
  }

  stage_count = build_pipeline(argc, argv, stage_starts, stage_lens, KSH_MAX_STAGES);
  if(stage_count < 2){
    puts_line("pipe: syntax");
    return -1;
  }

  for(i = 0; i < stage_count - 1; i++){
    if(xv6_pipe(&pipe_r[i], &pipe_w[i]) != 0){
      puts_line("pipe: alloc failed");
      goto fail;
    }
  }

  for(i = 0; i < stage_count; i++){
    char *stage_exec[KSH_MAX_ARGS];
    int stage_exec_argc = 0;
    int base_in = (i == 0) ? 0 : pipe_r[i - 1];
    int base_out = (i == stage_count - 1) ? 1 : pipe_w[i];
    ksh_io_t base_io;
    ksh_io_t io;
    int sid = -1;
    int stage_argc = stage_lens[i];
    char **stage_argv = &argv[stage_starts[i]];

    base_io.in_fd = base_in;
    base_io.out_fd = base_out;
    base_io.err_fd = 2;
    if(parse_exec_and_redir(stage_argc, stage_argv, &base_io, stage_exec, KSH_MAX_ARGS, &stage_exec_argc, &io) != 0)
      goto fail;

    if(i > 0 && io.in_fd != base_in && pipe_r[i - 1] >= 3){
      xv6_close(pipe_r[i - 1]);
      pipe_r[i - 1] = -1;
    }
    if(i < stage_count - 1 && io.out_fd != base_out && pipe_w[i] >= 3){
      xv6_close(pipe_w[i]);
      pipe_w[i] = -1;
    }

    if(i == stage_count - 1 && !run_bg){
      final_rc =
        run_foreground_with_limits(stage_exec_argc, stage_exec, io.in_fd, io.out_fd, io.err_fd, max_heap_kb, max_runtime_ms);
    } else {
      if(spawn_background_ex(stage_exec_argc, stage_exec, io.in_fd, io.out_fd, io.err_fd, 1, run_bg ? 0 : 1, max_heap_kb,
                             max_runtime_ms, &sid) != 0)
      {
        close_io_custom_fds(&io, &base_io);
        goto fail;
      }
      jobs[njobs++] = sid;
    }
    close_io_custom_fds(&io, &base_io);

    if(i > 0 && pipe_r[i - 1] >= 3){
      xv6_close(pipe_r[i - 1]);
      pipe_r[i - 1] = -1;
    }
    if(i < stage_count - 1 && pipe_w[i] >= 3){
      xv6_close(pipe_w[i]);
      pipe_w[i] = -1;
    }
  }

  if(!run_bg){
    for(i = 0; i < njobs; i++){
      int ignore = 0;
      (void)wait_job_id(jobs[i], &ignore, 1);
    }
  }

  return final_rc;

fail:
  for(i = 0; i < KSH_MAX_STAGES - 1; i++){
    if(pipe_r[i] >= 3)
      xv6_close(pipe_r[i]);
    if(pipe_w[i] >= 3)
      xv6_close(pipe_w[i]);
  }
  return -1;
}

static int execute_external(int argc, char **argv, int run_bg, int max_heap_kb, uint32 max_runtime_ms)
{
  char *exec_argv[KSH_MAX_ARGS];
  int exec_argc = 0;
  int rc;
  ksh_io_t base_io;
  ksh_io_t io;

  if(find_pipe_pos(argc, argv) >= 0)
    return run_pipeline(argc, argv, run_bg, max_heap_kb, max_runtime_ms);

  base_io.in_fd = 0;
  base_io.out_fd = 1;
  base_io.err_fd = 2;

  if(parse_exec_and_redir(argc, argv, &base_io, exec_argv, KSH_MAX_ARGS, &exec_argc, &io) != 0)
    return -1;

  if(run_bg)
    rc = spawn_background_ex(exec_argc, exec_argv, io.in_fd, io.out_fd, io.err_fd, 0, 0, max_heap_kb, max_runtime_ms, 0);
  else
    rc = run_foreground_with_limits(exec_argc, exec_argv, io.in_fd, io.out_fd, io.err_fd, max_heap_kb, max_runtime_ms);

  close_io_custom_fds(&io, &base_io);
  return rc;
}

static void cmd_jobs(void)
{
  int i;
  int any = 0;

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  for(i = 0; i < KSH_MAX_JOBS; i++){
    if(!g_jobs[i].used)
      continue;
    k_printf("[%d] %s %s\r\n", g_jobs[i].id, g_jobs[i].done ? "done" : "running", g_jobs[i].cmd);
    any = 1;
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(!any)
    puts_line("jobs: empty");
}

static void cmd_ps(void)
{
  int i;
  int any = 0;

  k_printf("PID STATE EXIT REASON LIMIT(ms/kb) CMD\r\n");
  k_printf("0 RUN - - - ksh\r\n");

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  for(i = 0; i < KSH_MAX_JOBS; i++){
    if(!g_jobs[i].used)
      continue;
    if(g_jobs[i].done){
      k_printf("%d DONE %d %s %u/%d %s\r\n", g_jobs[i].id, g_jobs[i].exit_code, job_reason_str(g_jobs[i].reason),
               (unsigned)g_jobs[i].max_runtime_ms, g_jobs[i].max_heap_kb, g_jobs[i].cmd);
    } else {
      k_printf("%d RUN - - %u/%d %s\r\n", g_jobs[i].id, (unsigned)g_jobs[i].max_runtime_ms, g_jobs[i].max_heap_kb,
               g_jobs[i].cmd);
    }
    any = 1;
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(!any)
    k_printf("- NOJOBS -\r\n");
}

static void cmd_wait(int argc, char **argv)
{
  if(argc == 1){
    while(1){
      int i;
      int has_used = 0;
      int has_running = 0;
      if(g_jobs_lock)
        (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
      for(i = 0; i < KSH_MAX_JOBS; i++){
        if(!g_jobs[i].used)
          continue;
        has_used = 1;
        if(g_jobs[i].done)
          memset(&g_jobs[i], 0, sizeof(g_jobs[i]));
        else
          has_running = 1;
      }
      if(g_jobs_lock)
        (void)xSemaphoreGive(g_jobs_lock);
      if(!has_used || !has_running)
        break;
      enforce_job_limits();
      hal_delay_ms(10);
    }
    puts_line("wait: done");
    return;
  }

  if(argc == 2){
    uint32 id = 0;
    int exit_code = 0;
    if(parse_u32_dec(argv[1], &id) != 0){
      puts_line("wait: bad job id");
      return;
    }
    if(wait_job_id((int)id, &exit_code, 1) != 0){
      puts_line("wait: no such job");
      return;
    }
    k_printf("wait: done %u\r\n", (unsigned)exit_code);
    return;
  }

  puts_line("usage: wait [jobid]");
}

static void cmd_kill(int argc, char **argv)
{
  uint32 id = 0;

  if(argc != 2 || parse_u32_dec(argv[1], &id) != 0){
    puts_line("usage: kill <jobid>");
    return;
  }

  if(terminate_job_id((int)id, 137, JOB_REASON_KILLED) != 0){
    puts_line("kill: no such job");
    return;
  }

  puts_line("kill: ok");
}

static void cmd_fg(int argc, char **argv)
{
  uint32 id = 0;
  int exit_code = 0;

  if(argc != 2 || parse_u32_dec(argv[1], &id) != 0){
    puts_line("usage: fg <jobid>");
    return;
  }

  if(wait_job_id((int)id, &exit_code, 1) != 0){
    puts_line("fg: no such job");
    return;
  }

  k_printf("fg: done %u\r\n", (unsigned)exit_code);
}

static void cmd_ulimit(int argc, char **argv)
{
  uint32 v = 0;

  if(argc == 1){
    k_printf("ulimit: -t %u ms, -m %d kb\r\n", (unsigned)g_ulimit_ms, g_ulimit_heap_kb);
    return;
  }

  if(argc == 2){
    if(strcmp(argv[1], "-t") == 0){
      k_printf("%u\r\n", (unsigned)g_ulimit_ms);
      return;
    }
    if(strcmp(argv[1], "-m") == 0){
      k_printf("%d\r\n", g_ulimit_heap_kb);
      return;
    }
    puts_line("usage: ulimit [-t ms] [-m kb]");
    return;
  }

  if(argc == 3){
    if(parse_u32_dec(argv[2], &v) != 0){
      puts_line("ulimit: bad value");
      return;
    }
    if(strcmp(argv[1], "-t") == 0){
      g_ulimit_ms = v;
      return;
    }
    if(strcmp(argv[1], "-m") == 0){
      g_ulimit_heap_kb = (int)v;
      return;
    }
  }

  puts_line("usage: ulimit [-t ms] [-m kb]");
}

static void cmd_limit(int argc, char **argv, int run_bg)
{
  uint32 max_ms = 0;
  uint32 max_kb = 0;

  if(argc < 4){
    puts_line("usage: limit <ms> <heap_kb> <cmd...>");
    return;
  }
  if(parse_u32_dec(argv[1], &max_ms) != 0 || parse_u32_dec(argv[2], &max_kb) != 0){
    puts_line("limit: bad numeric args");
    return;
  }

  (void)execute_external(argc - 3, argv + 3, run_bg, (int)max_kb, max_ms);
}

static void cmd_time(int argc, char **argv, int run_bg)
{
  uint32 start;
  uint32 end;
  if(argc < 2){
    puts_line("usage: time <cmd...>");
    return;
  }

  start = (uint32)k_ticks();
  (void)dispatch_command(argc - 1, argv + 1, run_bg);
  end = (uint32)k_ticks();

  if(!run_bg)
    k_eprintf("time: %u ms\r\n", (unsigned)((end - start) * 10u));
}

static void cmd_pwd(void)
{
  char cwd[MAXPATH];
  if(xv6_getcwd(cwd, sizeof(cwd)) != 0){
    eputs_line("pwd: failed");
    return;
  }
  puts_line(cwd);
}

static void cmd_cd(int argc, char **argv)
{
  const char *path = 0;

  if(argc > 2){
    eputs_line("usage: cd [dir]");
    return;
  }
  if(argc == 2)
    path = argv[1];
  else
    path = env_get("HOME");
  if(path == 0 || path[0] == 0)
    path = "/";
  if(xv6_chdir(path) != 0){
    eputs_console("cd: failed: ");
    eputs_line(path);
    return;
  }
  env_sync_pwd();
}

static void cmd_env(void)
{
  int i;
  for(i = 0; i < KSH_MAX_ENV; i++){
    if(!g_env[i].used)
      continue;
    k_printf("%s=%s\r\n", g_env[i].key, g_env[i].val);
  }
}

static void cmd_export(int argc, char **argv)
{
  int i;

  if(argc == 1){
    cmd_env();
    return;
  }
  for(i = 1; i < argc; i++){
    char *eq = strchr(argv[i], '=');
    if(eq){
      char key[KSH_ENV_KEY];
      int n = (int)(eq - argv[i]);
      if(n <= 0 || n >= (int)sizeof(key)){
        eputs_console("export: bad name: ");
        eputs_line(argv[i]);
        continue;
      }
      memcpy(key, argv[i], (unsigned)n);
      key[n] = 0;
      if(env_set(key, eq + 1) != 0){
        eputs_console("export: bad assignment: ");
        eputs_line(argv[i]);
      }
    } else {
      if(env_set(argv[i], "") != 0){
        eputs_console("export: bad name: ");
        eputs_line(argv[i]);
      }
    }
  }
}

static void cmd_unset(int argc, char **argv)
{
  int i;
  if(argc < 2){
    eputs_line("usage: unset NAME...");
    return;
  }
  for(i = 1; i < argc; i++){
    if(strcmp(argv[i], "PWD") == 0)
      continue;
    env_unset(argv[i]);
  }
}

static void register_default_symbols(void)
{
  static const elf_host_symbol_t syms[] = {
    { "puts", (void *)puts },
    { "printf", (void *)printf },
    { "malloc", (void *)malloc },
    { "calloc", (void *)calloc },
    { "realloc", (void *)realloc },
    { "free", (void *)free },
    { "memset", (void *)memset },
    { "memcpy", (void *)memcpy },
    { "strlen", (void *)strlen },
    { "strcmp", (void *)strcmp },
    { "usleep", (void *)usleep },
    { "k_ticks", (void *)k_ticks },
    { "k_puts", (void *)k_puts },
    { "k_free_heap", (void *)k_free_heap },
    { "xv6fs_readdir_path", (void *)k_fs_readdir_path },
    { "xv6fs_read_file_alloc_path", (void *)xv6fs_read_file_alloc_path },
    { "xv6fs_write_file_path", (void *)xv6fs_write_file_path },
    { "xv6fs_mkdir_path", (void *)xv6fs_mkdir_path },
    { "xv6fs_unlink_path", (void *)xv6fs_unlink_path },
    { "xv6_open", (void *)xv6_open },
    { "xv6_dup", (void *)xv6_dup },
    { "xv6_read", (void *)xv6_read },
    { "xv6_write", (void *)xv6_write },
    { "xv6_close", (void *)xv6_close },
    { "xv6_chdir", (void *)xv6_chdir },
    { "xv6_getcwd", (void *)xv6_getcwd },
    { "xv6_ptsname", (void *)xv6_ptsname },
    { "xv6_pipe", (void *)xv6_pipe },
  };

  (void)elf_loader_register_host_symbols(syms, (int)(sizeof(syms) / sizeof(syms[0])));
}

static int is_builtin_command(const char *cmd)
{
  return (strcmp(cmd, "help") == 0 || strcmp(cmd, "reboot") == 0 || strcmp(cmd, "cd") == 0 ||
          strcmp(cmd, "pwd") == 0 || strcmp(cmd, "env") == 0 || strcmp(cmd, "export") == 0 ||
          strcmp(cmd, "unset") == 0 || strcmp(cmd, "ps") == 0 || strcmp(cmd, "jobs") == 0 ||
          strcmp(cmd, "wait") == 0 || strcmp(cmd, "kill") == 0 || strcmp(cmd, "fg") == 0 ||
          strcmp(cmd, "time") == 0 || strcmp(cmd, "ulimit") == 0 || strcmp(cmd, "limit") == 0);
}

static int dispatch_builtin_command(int argc, char **argv, int run_bg)
{
  if(strcmp(argv[0], "help") == 0){
    cmd_help();
    return 0;
  }
  if(strcmp(argv[0], "reboot") == 0){
    puts_line("rebooting...");
    hal_reboot();
    return 0;
  }
  if(strcmp(argv[0], "cd") == 0){
    cmd_cd(argc, argv);
    return 0;
  }
  if(strcmp(argv[0], "pwd") == 0){
    cmd_pwd();
    return 0;
  }
  if(strcmp(argv[0], "env") == 0){
    cmd_env();
    return 0;
  }
  if(strcmp(argv[0], "export") == 0){
    cmd_export(argc, argv);
    return 0;
  }
  if(strcmp(argv[0], "unset") == 0){
    cmd_unset(argc, argv);
    return 0;
  }
  if(strcmp(argv[0], "ps") == 0){
    cmd_ps();
    return 0;
  }
  if(strcmp(argv[0], "jobs") == 0){
    cmd_jobs();
    return 0;
  }
  if(strcmp(argv[0], "wait") == 0){
    cmd_wait(argc, argv);
    return 0;
  }
  if(strcmp(argv[0], "kill") == 0){
    cmd_kill(argc, argv);
    return 0;
  }
  if(strcmp(argv[0], "fg") == 0){
    cmd_fg(argc, argv);
    return 0;
  }
  if(strcmp(argv[0], "time") == 0){
    cmd_time(argc, argv, run_bg);
    return 0;
  }
  if(strcmp(argv[0], "ulimit") == 0){
    cmd_ulimit(argc, argv);
    return 0;
  }
  if(strcmp(argv[0], "limit") == 0){
    cmd_limit(argc, argv, run_bg);
    return 0;
  }
  return -1;
}

static int dispatch_command(int argc, char **argv, int run_bg)
{
  char *cmd_argv[KSH_MAX_ARGS];
  int cmd_argc = 0;
  ksh_io_t base_io;
  ksh_io_t io;

  if(argc <= 0)
    return 0;

  if(is_builtin_command(argv[0])){
    base_io.in_fd = 0;
    base_io.out_fd = 1;
    base_io.err_fd = 2;
    if(parse_exec_and_redir(argc, argv, &base_io, cmd_argv, KSH_MAX_ARGS, &cmd_argc, &io) != 0)
      return 0;
    xv6_stdio_set_fds(io.in_fd, io.out_fd, io.err_fd);
    (void)dispatch_builtin_command(cmd_argc, cmd_argv, run_bg);
    xv6_stdio_reset_fds();
    close_io_custom_fds(&io, &base_io);
    return 0;
  }

  (void)execute_external(argc, argv, run_bg, g_ulimit_heap_kb, g_ulimit_ms);
  return 0;
}

void ksh_run(void)
{
  char line[256];
  int len = 0;

  elf_loader_init();
  g_jobs_lock = xSemaphoreCreateMutex();
  g_loader_lock = xSemaphoreCreateMutex();
  register_default_symbols();
  xv6_vfs_reset();
  (void)xv6_chdir("/");
  env_init_defaults();

  puts_line("xv6-esp32s3 ksh ready");
  cmd_help();
  tty_puts("xv6> ");

  while(1){
    int c;

    enforce_job_limits();

    c = hal_console_getc();
    if(c < 0){
      hal_delay_ms(5);
      continue;
    }

    if(c == 0x03){
      puts_line("^C");
      len = 0;
      tty_puts("xv6> ");
      continue;
    }

    if(c == '\r' || c == '\n'){
      char *argv[KSH_MAX_ARGS];
      int argc;
      int run_bg = 0;

      line[len] = 0;
      puts_line("");

      argc = parse_line(line, argv, KSH_MAX_ARGS);
      if(argc < 0){
        puts_line("parse: unterminated quote");
        tty_puts("xv6> ");
        len = 0;
        continue;
      }
      if(argc == 0){
        tty_puts("xv6> ");
        len = 0;
        continue;
      }

      if(strcmp(argv[argc - 1], "&") == 0){
        run_bg = 1;
        argc--;
        if(argc == 0){
          puts_line("syntax: command &");
          tty_puts("xv6> ");
          len = 0;
          continue;
        }
      }

      if(dispatch_command(argc, argv, run_bg) != 0)
        puts_line("unknown command");

      len = 0;
      tty_puts("xv6> ");
      continue;
    }

    if(c == 0x7f || c == '\b'){
      if(len > 0){
        len--;
        tty_puts("\b \b");
      }
      continue;
    }

    if(len < (int)(sizeof(line) - 1)){
      line[len++] = (char)c;
      tty_putc(c);
    }
  }
}
