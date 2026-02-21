#include "ksh.h"

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
  while(*s){
    hal_console_putc(*s++);
  }
  hal_console_putc('\r');
  hal_console_putc('\n');
  return 0;
}

static int k_fs_readdir_path(const char *path, int index, char *name_out, int name_out_len, uint16 *type_out,
                             uint32 *size_out)
{
  return xv6fs_list_path(path, index, name_out, name_out_len, type_out, size_out);
}

#define KSH_MAX_JOBS 8

typedef struct {
  int used;
  int id;
  int done;
  int exit_code;
  char cmd[96];
} ksh_job_t;

typedef struct {
  int slot;
  int argc;
  char **argv;
} ksh_job_task_t;

static ksh_job_t g_jobs[KSH_MAX_JOBS];
static int g_next_job_id = 1;
static SemaphoreHandle_t g_jobs_lock;
static SemaphoreHandle_t g_loader_lock;

static void putc_console(int c)
{
  hal_console_putc(c);
}

static void puts_console(const char *s)
{
  while(*s != 0){
    putc_console(*s);
    s++;
  }
}

static void puts_line(const char *s)
{
  puts_console(s);
  puts_console("\r\n");
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

static void print_u64(uint64 v)
{
  char tmp[21];
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

static int split(char *line, char **argv, int max_args)
{
  int argc = 0;
  char *p = line;

  while(*p != 0 && argc < max_args){
    while(*p == ' ' || *p == '\t')
      p++;
    if(*p == 0)
      break;
    argv[argc++] = p;
    while(*p != 0 && *p != ' ' && *p != '\t')
      p++;
    if(*p == 0)
      break;
    *p = 0;
    p++;
  }

  return argc;
}

static void cmd_help(void)
{
  puts_line("commands:");
  puts_line("  help");
  puts_line("  <elf-command> [args]");
  puts_line("  <elf-command> [args] &");
  puts_line("  jobs");
  puts_line("  wait [jobid]");
  puts_line("  reboot");
}

static int run_elf_command(int argc, char **argv, int *exit_code)
{
  elf_module_t *m;
  void *image = 0;
  uint32 image_size = 0;
  int retv = 0;
  char pathbuf[MAXPATH];
  const char *paths[] = { "/bin", "/usr/bin", "/" };
  int pi;

  if(argc <= 0 || argv == 0 || argv[0] == 0 || argv[0][0] == 0)
    return -1;

  if(exit_code)
    *exit_code = 127;

  if(g_loader_lock)
    (void)xSemaphoreTake(g_loader_lock, portMAX_DELAY);
  m = elf_module_find(argv[0]);
  if(m == 0){
    if(strchr(argv[0], '/')){
      if(xv6fs_read_file_alloc_path(argv[0], &image, &image_size) != 0){
        if(snprintf(pathbuf, sizeof(pathbuf), "%s.elf", argv[0]) > 0)
          (void)xv6fs_read_file_alloc_path(pathbuf, &image, &image_size);
      }
    } else {
      for(pi = 0; pi < (int)(sizeof(paths) / sizeof(paths[0])); pi++){
        int n = snprintf(pathbuf, sizeof(pathbuf), "%s/%s", paths[pi], argv[0]);
        if(n <= 0 || n >= (int)sizeof(pathbuf))
          continue;
        if(xv6fs_read_file_alloc_path(pathbuf, &image, &image_size) == 0)
          break;
        if(snprintf(pathbuf, sizeof(pathbuf), "%s/%s.elf", paths[pi], argv[0]) > 0 &&
           xv6fs_read_file_alloc_path(pathbuf, &image, &image_size) == 0)
          break;
      }
    }
    if(image == 0){
      if(g_loader_lock)
        (void)xSemaphoreGive(g_loader_lock);
      puts_line("exec: command not found");
      return -1;
    }
    if(elf_module_load_from_bytes(argv[0], image, image_size, &m) != 0){
      free(image);
      if(g_loader_lock)
        (void)xSemaphoreGive(g_loader_lock);
      puts_line("exec: elf load failed");
      return -1;
    }
    free(image);
  }
  if(g_loader_lock)
    (void)xSemaphoreGive(g_loader_lock);

  if(elf_module_call_main(m, argc, argv, &retv) != 0){
    puts_line("exec: entry call failed");
    if(exit_code)
      *exit_code = 126;
    return -1;
  }
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

static void job_print_one(const ksh_job_t *j)
{
  putc_console('[');
  print_u32((uint32)j->id);
  puts_console("] ");
  if(j->done){
    puts_console("done ");
    print_u32((uint32)j->exit_code);
    putc_console(' ');
  } else {
    puts_console("running ");
  }
  puts_line(j->cmd);
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
    job_print_one(&g_jobs[i]);
    any = 1;
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);
  if(!any)
    puts_line("jobs: empty");
}

static void job_task(void *arg)
{
  int i;
  int exit_code = 127;
  ksh_job_task_t *t = (ksh_job_task_t *)arg;

  (void)run_elf_command(t->argc, t->argv, &exit_code);
  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  if(t->slot >= 0 && t->slot < KSH_MAX_JOBS && g_jobs[t->slot].used){
    g_jobs[t->slot].done = 1;
    g_jobs[t->slot].exit_code = exit_code;
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  for(i = 0; i < t->argc; i++)
    free(t->argv[i]);
  free(t->argv);
  free(t);
  vTaskDelete(NULL);
}

static int spawn_background(int argc, char **argv)
{
  int i;
  int slot = -1;
  int id = 0;
  int pos = 0;
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
  t->argc = argc;
  t->slot = slot;
  for(i = 0; i < argc; i++){
    size_t n = strlen(argv[i]) + 1;
    t->argv[i] = (char *)malloc(n);
    if(t->argv[i] == 0){
      int j;
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

  if(xTaskCreate(job_task, "xv6_bg", 6144, t, 5, 0) != pdPASS){
    if(g_jobs_lock)
      (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
    memset(&g_jobs[slot], 0, sizeof(g_jobs[slot]));
    if(g_jobs_lock)
      (void)xSemaphoreGive(g_jobs_lock);
    for(i = 0; i < argc; i++)
      free(t->argv[i]);
    free(t->argv);
    free(t);
    puts_line("jobs: spawn failed");
    return -1;
  }

  putc_console('[');
  print_u32((uint32)id);
  puts_line("] started");
  return 0;
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
      hal_delay_ms(10);
    }
    puts_line("wait: done");
    return;
  }

  if(argc == 2){
    uint32 id = 0;
    if(parse_u32_dec(argv[1], &id) != 0){
      puts_line("wait: bad job id");
      return;
    }
    while(1){
      int slot;
      int done = 0;
      int exit_code = 0;
      if(g_jobs_lock)
        (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
      slot = job_find_slot_by_id((int)id);
      if(slot < 0){
        if(g_jobs_lock)
          (void)xSemaphoreGive(g_jobs_lock);
        puts_line("wait: no such job");
        return;
      }
      if(g_jobs[slot].done){
        done = 1;
        exit_code = g_jobs[slot].exit_code;
        memset(&g_jobs[slot], 0, sizeof(g_jobs[slot]));
      }
      if(g_jobs_lock)
        (void)xSemaphoreGive(g_jobs_lock);
      if(done){
        puts_console("wait: done ");
        print_u32((uint32)exit_code);
        puts_line("");
        return;
      }
      hal_delay_ms(10);
    }
  }

  puts_line("usage: wait [jobid]");
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
    { "xv6_read", (void *)xv6_read },
    { "xv6_write", (void *)xv6_write },
    { "xv6_close", (void *)xv6_close },
    { "xv6_ptsname", (void *)xv6_ptsname },
  };
  (void)elf_loader_register_host_symbols(syms, (int)(sizeof(syms) / sizeof(syms[0])));
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

  puts_line("xv6-esp32s3 ksh ready");
  cmd_help();
  puts_console("xv6> ");

  while(1){
    int c = hal_console_getc();
    if(c < 0){
      hal_delay_ms(5);
      continue;
    }

    if(c == '\r' || c == '\n'){
      char *argv[16];
      int argc;
      int run_bg = 0;
      line[len] = 0;
      puts_line("");

      argc = split(line, argv, 16);
      if(argc == 0){
        puts_console("xv6> ");
        len = 0;
        continue;
      }
      if(argc > 0 && strcmp(argv[argc - 1], "&") == 0){
        run_bg = 1;
        argc--;
        if(argc == 0){
          puts_line("syntax: command &");
          puts_console("xv6> ");
          len = 0;
          continue;
        }
      }

      if(strcmp(argv[0], "help") == 0){
        cmd_help();
      } else if(strcmp(argv[0], "jobs") == 0){
        cmd_jobs();
      } else if(strcmp(argv[0], "wait") == 0){
        cmd_wait(argc, argv);
      } else if(strcmp(argv[0], "reboot") == 0){
        puts_line("rebooting...");
        hal_reboot();
      } else {
        if(run_bg){
          (void)spawn_background(argc, argv);
        } else if(run_elf_command(argc, argv, 0) != 0){
          puts_line("unknown command");
        }
      }

      len = 0;
      puts_console("xv6> ");
      continue;
    }

    if(c == 0x7f || c == '\b'){
      if(len > 0){
        len--;
        puts_console("\b \b");
      }
      continue;
    }

    if(len < (int)(sizeof(line) - 1)){
      line[len++] = (char)c;
      putc_console(c);
    }
  }
}
