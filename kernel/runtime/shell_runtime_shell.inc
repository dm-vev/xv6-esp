static int k_fs_readdir_path(const char *path, int index, char *name_out, int name_out_len, uint16 *type_out,
                             uint32 *size_out)
{
  const char *path_host = (const char *)elf_loader_translate_ptr(path);
  char *name_host = (char *)elf_loader_translate_ptr(name_out);
  uint16 *type_host = (uint16 *)elf_loader_translate_ptr(type_out);
  uint32 *size_host = (uint32 *)elf_loader_translate_ptr(size_out);

  if(path_host == 0 || name_host == 0 || name_out_len <= 1 || name_out_len > MAXPATH){
    errno = EINVAL;
    return -1;
  }
  if(!k_ptr_bytes_accessible(name_host, (size_t)name_out_len)){
    errno = EINVAL;
    return -1;
  }
  if(type_out && (type_host == 0 || !k_ptr_bytes_accessible(type_host, sizeof(*type_host)))){
    errno = EINVAL;
    return -1;
  }
  if(size_out && (size_host == 0 || !k_ptr_bytes_accessible(size_host, sizeof(*size_host)))){
    errno = EINVAL;
    return -1;
  }
  return xv6fs_list_path(path_host, index, name_host, name_out_len, type_host, size_host);
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
  int rc = xv6_write(1, &ch, 1);
  if(rc < 0)
    tty_putc(c);
}

static void puts_console(const char *s)
{
  int n;
  if(s == 0)
    return;
  n = (int)strlen(s);
  if(n <= 0)
    return;
  if(xv6_stdio_is_default_out()){
    (void)fwrite(s, 1, (size_t)n, stdout);
    return;
  }
  if(xv6_write(1, s, (uint32)n) == n)
    return;
  while(*s)
    tty_putc(*s++);
}

static void eputs_console(const char *s)
{
  int n;
  if(s == 0)
    return;
  n = (int)strlen(s);
  if(n <= 0)
    return;
  if(xv6_stdio_is_default_out()){
    (void)fwrite(s, 1, (size_t)n, stderr);
    return;
  }
  if(xv6_write(2, s, (uint32)n) == n)
    return;
  while(*s)
    hal_console_putc(*s++);
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

static esp_err_t k_wifi_host_init_default(void)
{
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  return esp_wifi_init(&cfg);
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
    k_copy_cstr(g_env[i].key, sizeof(g_env[i].key), key);
  }
  k_copy_cstr(g_env[i].val, sizeof(g_env[i].val), val);
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
  case JOB_REASON_STOPPED:
    return "stopped";
  default:
    return "-";
  }
}


static int cmd_help(void)
{
  puts_line("commands:");
  puts_line("  help");
  puts_line("  <elf-command> [args]");
  puts_line("  <elf-command> [args] &");
  puts_line("  <a> | <b> | <c> ...");
  puts_line("  redirection: < > >> 2> 2>>");
  puts_line("  cd [dir], pwd");
  puts_line("  env, export NAME=VALUE, unset NAME");
  puts_line("  health");
  puts_line("  ps");
  puts_line("  jobs");
  puts_line("  fg <jobid>");
  puts_line("  bg <jobid>");
  puts_line("  kill <jobid>");
  puts_line("  wait [jobid]");
  puts_line("  time <cmd...>");
  puts_line("  ulimit [-t ms] [-m kb]");
  puts_line("  limit <ms> <heap_kb> <cmd...> [&]");
  puts_line("  kmod <list|load|unload|reload|autoload|verify> ...");
  puts_line("  reboot");
  return 0;
}

static void set_resolved_path(char *resolved, int resolved_len, const char *path)
{
  if(resolved && resolved_len > 0){
    strncpy(resolved, path, resolved_len - 1);
    resolved[resolved_len - 1] = 0;
  }
}

static int try_read_exec_path(const char *path, void **out_image, uint32 *out_size, char *resolved, int resolved_len)
{
  if(path == 0 || path[0] == 0)
    return -1;
  if(xv6fs_read_file_alloc_path(path, out_image, out_size) != 0)
    return -1;
  set_resolved_path(resolved, resolved_len, path);
  return 0;
}

static int try_read_exec_image(const char *cmd, void **out_image, uint32 *out_size, char *resolved, int resolved_len)
{
  char pathbuf[MAXPATH];
  const char *path_env;
  const char *p;
  int n;
  static const char *k_default_path = "/bin:/home/bin";

  if(cmd == 0 || out_image == 0 || out_size == 0)
    return -1;
  *out_image = 0;
  *out_size = 0;
  if(resolved && resolved_len > 0)
    resolved[0] = 0;

  if(strchr(cmd, '/')){
    if(try_read_exec_path(cmd, out_image, out_size, resolved, resolved_len) == 0)
      return 0;
    n = snprintf(pathbuf, sizeof(pathbuf), "%s.so", cmd);
    if(n > 0 && n < (int)sizeof(pathbuf) && try_read_exec_path(pathbuf, out_image, out_size, resolved, resolved_len) == 0)
      return 0;
    n = snprintf(pathbuf, sizeof(pathbuf), "%s.elf", cmd);
    if(n > 0 && n < (int)sizeof(pathbuf) && try_read_exec_path(pathbuf, out_image, out_size, resolved, resolved_len) == 0)
      return 0;
    return -1;
  }

  path_env = env_get("PATH");
  if(path_env == 0 || path_env[0] == 0)
    path_env = k_default_path;

  p = path_env;
  while(1){
    const char *seg = p;
    int seg_len = 0;
    int remain;

    while(*p && *p != ':'){
      p++;
      seg_len++;
    }

    if(seg_len == 0){
      n = snprintf(pathbuf, sizeof(pathbuf), "%s", cmd);
      if(n > 0 && n < (int)sizeof(pathbuf) && try_read_exec_path(pathbuf, out_image, out_size, resolved, resolved_len) == 0)
        return 0;
      n = snprintf(pathbuf, sizeof(pathbuf), "%s.so", cmd);
      if(n > 0 && n < (int)sizeof(pathbuf) && try_read_exec_path(pathbuf, out_image, out_size, resolved, resolved_len) == 0)
        return 0;
      n = snprintf(pathbuf, sizeof(pathbuf), "%s.elf", cmd);
      if(n > 0 && n < (int)sizeof(pathbuf) && try_read_exec_path(pathbuf, out_image, out_size, resolved, resolved_len) == 0)
        return 0;
    } else {
      if(seg_len >= (int)sizeof(pathbuf))
        goto next_seg;
      memcpy(pathbuf, seg, (unsigned)seg_len);
      pathbuf[seg_len] = 0;
      remain = (int)sizeof(pathbuf) - seg_len;

      n = snprintf(pathbuf + seg_len, (size_t)remain, "/%s", cmd);
      if(n > 0 && n < remain && try_read_exec_path(pathbuf, out_image, out_size, resolved, resolved_len) == 0)
        return 0;
      n = snprintf(pathbuf + seg_len, (size_t)remain, "/%s.so", cmd);
      if(n > 0 && n < remain && try_read_exec_path(pathbuf, out_image, out_size, resolved, resolved_len) == 0)
        return 0;
      n = snprintf(pathbuf + seg_len, (size_t)remain, "/%s.elf", cmd);
      if(n > 0 && n < remain && try_read_exec_path(pathbuf, out_image, out_size, resolved, resolved_len) == 0)
        return 0;
    }

next_seg:
    if(*p == 0)
      break;
    p++;
  }

  return -1;
}

static char **dup_exec_argv(int argc, char **argv)
{
  char **copy;
  int i;

  if(argc <= 0 || argv == 0)
    return 0;

  copy = (char **)calloc((size_t)argc + 1u, sizeof(char *));
  if(copy == 0)
    return 0;

  for(i = 0; i < argc; i++){
    copy[i] = strdup(argv[i] ? argv[i] : "");
    if(copy[i] == 0){
      int j;
      for(j = 0; j < i; j++)
        free(copy[j]);
      free(copy);
      return 0;
    }
  }
  copy[argc] = 0;
  return copy;
}

static void free_exec_argv(int argc, char **argv)
{
  int i;
  if(argv == 0)
    return;
  for(i = 0; i < argc; i++)
    free(argv[i]);
  free(argv);
}

static char **dup_exec_envp(int *out_count)
{
  char **envp;
  int i;
  int n = 0;

  if(out_count)
    *out_count = 0;
  for(i = 0; i < KSH_MAX_ENV; i++){
    if(g_env[i].used)
      n++;
  }

  envp = (char **)calloc((size_t)n + 1u, sizeof(char *));
  if(envp == 0)
    return 0;

  n = 0;
  for(i = 0; i < KSH_MAX_ENV; i++){
    int key_len;
    int val_len;
    int total_len;
    char *kv;

    if(!g_env[i].used)
      continue;
    key_len = (int)strlen(g_env[i].key);
    val_len = (int)strlen(g_env[i].val);
    total_len = key_len + 1 + val_len;
    kv = (char *)malloc((size_t)total_len + 1u);
    if(kv == 0){
      int j;
      for(j = 0; j < n; j++)
        free(envp[j]);
      free(envp);
      return 0;
    }
    memcpy(kv, g_env[i].key, (size_t)key_len);
    kv[key_len] = '=';
    memcpy(kv + key_len + 1, g_env[i].val, (size_t)val_len);
    kv[total_len] = 0;
    envp[n++] = kv;
  }
  envp[n] = 0;
  if(out_count)
    *out_count = n;
  return envp;
}

static void free_exec_envp(int envc, char **envp)
{
  int i;
  if(envp == 0)
    return;
  for(i = 0; i < envc; i++)
    free(envp[i]);
  free(envp);
}

typedef struct {
  int active;
  int in_fd;
  int out_fd;
  int err_fd;
} ksh_stdio_state_t;

static void ksh_stdio_state_capture(ksh_stdio_state_t *state)
{
  if(state == 0)
    return;
  xv6_stdio_get_fds(&state->in_fd, &state->out_fd, &state->err_fd, &state->active);
}

static void ksh_stdio_state_restore(const ksh_stdio_state_t *state)
{
  if(state == 0)
    return;
  if(state->active)
    (void)xv6_stdio_set_fds(state->in_fd, state->out_fd, state->err_fd);
  else
    xv6_stdio_reset_fds();
}

static int run_elf_command(int argc, char **argv, int *exit_code, int in_fd, int out_fd, int err_fd, int max_heap_kb)
{
  elf_module_t *m;
  void *image = 0;
  uint32 image_size = 0;
  int retv = 0;
  int rc = -1;
  char **exec_argv_owned = 0;
  char **exec_argv = 0;
  char **exec_envp = 0;
  char **saved_environ = 0;
  int exec_envc = 0;
  char module_name[MAXPATH];
  int module_loaded = 0;
  int environ_swapped = 0;
  int stdio_swapped = 0;
  ksh_stdio_state_t stdio_saved;

  if(argc <= 0 || argv == 0 || argv[0] == 0 || argv[0][0] == 0)
    return -1;

  /*
   * LWIP in ESP-IDF uses per-thread netconn semaphores for socket API.
   * Ensure current task has it initialized before any applet socket usage.
   */
  netconn_thread_init();

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

  exec_argv_owned = dup_exec_argv(argc, argv);
  if(exec_argv_owned == 0){
    puts_line("exec: no memory");
    return -1;
  }
  exec_argv = (char **)calloc((size_t)argc + 1u, sizeof(char *));
  if(exec_argv == 0){
    free_exec_argv(argc, exec_argv_owned);
    puts_line("exec: no memory");
    return -1;
  }
  memcpy(exec_argv, exec_argv_owned, ((size_t)argc + 1u) * sizeof(char *));
  exec_envp = dup_exec_envp(&exec_envc);
  if(exec_envp == 0){
    free(exec_argv);
    free_exec_argv(argc, exec_argv_owned);
    puts_line("exec: no memory");
    return -1;
  }
  k_optind = 1;
  k_opterr = 1;
  k_optopt = 0;
  k_optarg = 0;
  k_optreset = 0;
  k_getopt_pos = 1;

  /*
   * Keep libc global environment in sync with the applet envp so
   * stdlib calls (getenv/environ-based tools like printenv) behave
   * consistently inside loaded ELF modules.
   */
  saved_environ = environ;
  environ = exec_envp;
  environ_swapped = 1;

  ksh_stdio_state_capture(&stdio_saved);
  if(xv6_stdio_set_fds(in_fd, out_fd, err_fd) != 0){
    puts_line("exec: no task context");
    goto out;
  }
  stdio_swapped = 1;

  strncpy(module_name, exec_argv[0], sizeof(module_name) - 1);
  module_name[sizeof(module_name) - 1] = 0;
  if(try_read_exec_image(exec_argv[0], &image, &image_size, module_name, sizeof(module_name)) != 0 || image == 0){
    puts_line("exec: command not found");
    goto out;
  }
  if(elf_module_load_from_owned_bytes(module_name, image, image_size, &m) != 0){
    puts_line("exec: elf load failed");
    goto out;
  }
  (void)elf_module_set_global(m, 0);
  module_loaded = 1;

  if(elf_module_call_main_ex(m, argc, exec_argv, exec_envp, &retv) != 0){
    puts_line("exec: entry call failed");
    if(exit_code)
      *exit_code = 126;
    goto out;
  }
  if(exit_code)
    *exit_code = retv;
  if(retv != 0){
    puts_console(exec_argv[0]);
    puts_console(": exit=");
    print_u32((uint32)retv);
    puts_line("");
  }
  rc = 0;

out:
  if(environ_swapped)
    environ = saved_environ;
  if(module_loaded){
    (void)elf_module_unload(module_name);
  }
  if(stdio_swapped)
    ksh_stdio_state_restore(&stdio_saved);
  free_exec_envp(exec_envc, exec_envp);
  free(exec_argv);
  free_exec_argv(argc, exec_argv_owned);
  return rc;
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

static int job_is_user_visible_id(int id)
{
  int slot;
  int visible = 0;

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  slot = job_find_slot_by_id(id);
  if(slot >= 0 && g_jobs[slot].used && g_jobs[slot].user_visible)
    visible = 1;
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);
  return visible;
}

static int job_id_in_use_locked(int id)
{
  int i;
  for(i = 0; i < KSH_MAX_JOBS; i++){
    if(g_jobs[i].used && g_jobs[i].id == id)
      return 1;
  }
  return 0;
}

static int alloc_job_id_locked(void)
{
  int attempts = KSH_MAX_JOBS + 1;

  if(g_next_job_id <= 1)
    g_next_job_id = 2;
  while(attempts-- > 0){
    int id = g_next_job_id++;
    if(g_next_job_id <= 1)
      g_next_job_id = 2;
    if(id == g_shell_pid)
      continue;
    if(!job_id_in_use_locked(id))
      return id;
  }
  return -1;
}

static int runtime_core_count(void)
{
#if defined(configNUMBER_OF_CORES)
  return (int)configNUMBER_OF_CORES;
#elif defined(portNUM_PROCESSORS)
  return (int)portNUM_PROCESSORS;
#else
  return 1;
#endif
}

static int current_core_id(void)
{
  BaseType_t core = xPortGetCoreID();
  if(core < 0 || core >= runtime_core_count())
    return -1;
  return (int)core;
}

static int select_job_core_locked(void)
{
  int i;
  int start;
  int core_count = runtime_core_count();
  int loads[KSH_MAX_CORES];
  int best_core;
  int best_load;

  if(core_count <= 1)
    return 0;
  if(core_count > KSH_MAX_CORES)
    core_count = KSH_MAX_CORES;

  memset(loads, 0, sizeof(loads));
  for(i = 0; i < KSH_MAX_JOBS; i++){
    int core;
    if(!g_jobs[i].used || g_jobs[i].done)
      continue;
    core = g_jobs[i].assigned_core;
    if(core < 0 || core >= core_count)
      continue;
    loads[core]++;
  }

  start = g_next_core_hint % core_count;
  if(start < 0)
    start = 0;
  best_core = start;
  best_load = loads[start];
  for(i = 1; i < core_count; i++){
    int c = (start + i) % core_count;
    if(loads[c] < best_load){
      best_load = loads[c];
      best_core = c;
    }
  }

  g_next_core_hint = (best_core + 1) % core_count;
  return best_core;
}

static void reap_finished_jobs_locked(void)
{
  int i;
  for(i = 0; i < KSH_MAX_JOBS; i++){
    if(!g_jobs[i].used)
      continue;
    if(g_jobs[i].done && g_jobs[i].task == 0 && g_jobs[i].task_ctx == 0)
      memset(&g_jobs[i], 0, sizeof(g_jobs[i]));
  }
}

static void close_job_fd_if_needed(int fd)
{
  if(fd >= 3)
    (void)hostabi_posix_fs_close(fd);
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
  if(t->argv){
    for(i = 0; i < t->argc; i++)
      free(t->argv[i]);
    free(t->argv);
  }
  free(t);
}

static void enforce_job_limits(void)
{
  int i;
  uint32 now = (uint32)k_ticks();
  ksh_job_task_t *kill_ctx[KSH_MAX_JOBS];
  int nkill = 0;
  int post_sigchld = 0;

  memset(kill_ctx, 0, sizeof(kill_ctx));

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  for(i = 0; i < KSH_MAX_JOBS; i++){
    uint32 elapsed_ms;
    if(!g_jobs[i].used || g_jobs[i].done)
      continue;
    elapsed_ms = k_ticks_to_ms_u32((uint32)(now - g_jobs[i].started_ms));
    if(g_jobs[i].max_runtime_ms > 0 && elapsed_ms > g_jobs[i].max_runtime_ms){
      TaskHandle_t h = g_jobs[i].task;
      if(h){
        vTaskDelete(h);
        xv6_task_ctx_cleanup_for_handle((void *)h);
        k_signal_task_cleanup(h);
      }
      if(nkill < KSH_MAX_JOBS && g_jobs[i].task_ctx)
        kill_ctx[nkill++] = (ksh_job_task_t *)g_jobs[i].task_ctx;
      g_jobs[i].done = 1;
      g_jobs[i].stopped = 0;
      g_jobs[i].wait_pending_exit = 1;
      g_jobs[i].wait_pending_stop = 0;
      g_jobs[i].wait_pending_cont = 0;
      g_jobs[i].exit_code = 124;
      g_jobs[i].reason = JOB_REASON_TIMEOUT;
      g_jobs[i].task = 0;
      g_jobs[i].task_ctx = 0;
      post_sigchld = 1;
    }
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  for(i = 0; i < nkill; i++){
    if(kill_ctx[i])
      free_job_ctx(kill_ctx[i]);
  }
  if(post_sigchld)
    k_signal_post_sigchld();
}

static void job_task(void *arg)
{
  ksh_job_task_t *t = (ksh_job_task_t *)arg;
  int exit_code = 127;
  int running_core = current_core_id();
  TaskHandle_t self_task = xTaskGetCurrentTaskHandle();

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  if(t->slot >= 0 && t->slot < KSH_MAX_JOBS && g_jobs[t->slot].used){
    g_jobs[t->slot].task = self_task;
    g_jobs[t->slot].running_core = running_core;
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(t->cwd[0])
    (void)xv6_chdir(t->cwd);
  (void)run_elf_command(t->argc, t->argv, &exit_code, t->in_fd, t->out_fd, t->err_fd, t->max_heap_kb);
  netconn_thread_cleanup();

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  if(t->slot >= 0 && t->slot < KSH_MAX_JOBS && g_jobs[t->slot].used){
    g_jobs[t->slot].done = 1;
    g_jobs[t->slot].stopped = 0;
    g_jobs[t->slot].wait_pending_exit = 1;
    g_jobs[t->slot].wait_pending_stop = 0;
    g_jobs[t->slot].wait_pending_cont = 0;
    g_jobs[t->slot].exit_code = exit_code;
    g_jobs[t->slot].reason = (g_jobs[t->slot].reason == JOB_REASON_NONE) ? JOB_REASON_EXIT : g_jobs[t->slot].reason;
    g_jobs[t->slot].task = 0;
    g_jobs[t->slot].task_ctx = 0;
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  free_job_ctx(t);
  k_signal_post_sigchld();
  k_signal_task_cleanup(self_task);
  xv6_task_ctx_cleanup();
  vTaskDelete(NULL);
}

static int terminate_job_id(int id, int exit_code, int reason)
{
  int slot;
  TaskHandle_t task = 0;
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  ksh_job_task_t *ctx = 0;
  int self_delete = 0;
  int rc = -1;

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  slot = job_find_slot_by_id(id);
  if(slot >= 0 && g_jobs[slot].used && !g_jobs[slot].done){
    task = g_jobs[slot].task;
    ctx = (ksh_job_task_t *)g_jobs[slot].task_ctx;
    if(task && task != self){
      vTaskDelete(task);
      xv6_task_ctx_cleanup_for_handle((void *)task);
      k_signal_task_cleanup(task);
    } else if(task && task == self){
      self_delete = 1;
    }
    g_jobs[slot].done = 1;
    g_jobs[slot].stopped = 0;
    g_jobs[slot].wait_pending_exit = 1;
    g_jobs[slot].wait_pending_stop = 0;
    g_jobs[slot].wait_pending_cont = 0;
    g_jobs[slot].exit_code = exit_code;
    g_jobs[slot].reason = reason;
    g_jobs[slot].task = 0;
    g_jobs[slot].task_ctx = 0;
    rc = 0;
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(rc != 0)
    return -1;
  if(ctx)
    free_job_ctx(ctx);
  k_signal_post_sigchld();
  if(self_delete){
    netconn_thread_cleanup();
    k_signal_task_cleanup(self);
    xv6_task_ctx_cleanup();
    vTaskDelete(NULL);
    for(;;){
    }
  }
  return rc;
}

static int suspend_job_id(int id)
{
  int slot;
  TaskHandle_t task = 0;
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  int self_suspend = 0;
  int should_suspend = 0;
  int post_sigchld = 0;

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  slot = job_find_slot_by_id(id);
  if(slot >= 0 && g_jobs[slot].used && !g_jobs[slot].done){
    task = g_jobs[slot].task;
    if(task && !g_jobs[slot].stopped){
      g_jobs[slot].stopped = 1;
      g_jobs[slot].wait_pending_stop = 1;
      g_jobs[slot].wait_pending_cont = 0;
      g_jobs[slot].reason = JOB_REASON_STOPPED;
      g_jobs[slot].user_visible = 1;
      should_suspend = 1;
      post_sigchld = 1;
      if(task == self){
        self_suspend = 1;
      } else {
        vTaskSuspend(task);
      }
    }
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(task == 0){
    errno = ESRCH;
    return -1;
  }
  if(post_sigchld)
    k_signal_post_sigchld();
  if(should_suspend && self_suspend)
    vTaskSuspend(NULL);
  return 0;
}

static int resume_job_id(int id)
{
  int slot;
  TaskHandle_t task = 0;
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  int should_resume = 0;

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  slot = job_find_slot_by_id(id);
  if(slot >= 0 && g_jobs[slot].used && !g_jobs[slot].done){
    task = g_jobs[slot].task;
    if(task && g_jobs[slot].stopped){
      g_jobs[slot].stopped = 0;
      g_jobs[slot].wait_pending_cont = 1;
      g_jobs[slot].reason = JOB_REASON_NONE;
      should_resume = 1;
      if(task != self)
        vTaskResume(task);
    }
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(task == 0){
    errno = ESRCH;
    return -1;
  }
  if(should_resume && task == self){
    errno = EPERM;
    return -1;
  }
  return 0;
}

static int wait_job_id_ex(int id, int *out_exit_code, int consume, int allow_ctrl_c)
{
  int interrupt_pending = 0;
  int suspend_pending = 0;

  while(1){
    int slot;
    int done = 0;
    int stopped = 0;
    int exit_code = 0;
    int pgid = -1;
    int poll_ctrl = 0;
    int tty_sig = 0;

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
    } else {
      stopped = g_jobs[slot].stopped;
      pgid = g_jobs[slot].pgid;
    }
    if(g_jobs_lock)
      (void)xSemaphoreGive(g_jobs_lock);

    if(done){
      if(out_exit_code)
        *out_exit_code = exit_code;
      return 0;
    }
    if(stopped){
      if(out_exit_code)
        *out_exit_code = 128 + SIGTSTP;
      return 0;
    }

    poll_ctrl = (allow_ctrl_c && shell_allow_ctrl_c_poll()) ? 1 : 0;
    if(poll_ctrl)
      tty_sig = hostabi_posix_tty_poll_signal();

    if(poll_ctrl && (interrupt_pending || tty_sig == SIGINT)){
      int sig_rc = -1;
      interrupt_pending = 1;
      if(tty_sig == SIGINT)
        sig_rc = hostabi_posix_tty_dispatch_signal(SIGINT);
      if(sig_rc == 0 || (pgid > 0 && k_kill(-pgid, SIGINT) == 0) || terminate_job_id(id, 130, JOB_REASON_KILLED) == 0){
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
      interrupt_pending = 0;
    }

    if(poll_ctrl && (suspend_pending || tty_sig == SIGTSTP)){
      int sig_rc = -1;
      suspend_pending = 1;
      if(tty_sig == SIGTSTP)
        sig_rc = hostabi_posix_tty_dispatch_signal(SIGTSTP);
      if(sig_rc == 0 || (pgid > 0 && k_kill(-pgid, SIGTSTP) == 0) || suspend_job_id(id) == 0){
        puts_line("^Z");
        if(out_exit_code)
          *out_exit_code = 128 + SIGTSTP;
        return 0;
      }
      suspend_pending = 0;
    }

    enforce_job_limits();
    (void)k_signal_dispatch_pending_current();
    hal_delay_ms(10);
  }
}

static int wait_job_id(int id, int *out_exit_code, int consume)
{
  return wait_job_id_ex(id, out_exit_code, consume, 0);
}

static void cleanup_spawned_jobs(int *jobs, int njobs)
{
  int i;
  if(jobs == 0)
    return;
  for(i = 0; i < njobs; i++){
    if(jobs[i] <= 0)
      continue;
    (void)terminate_job_id(jobs[i], 130, JOB_REASON_KILLED);
    (void)wait_job_id(jobs[i], 0, 1);
  }
}

static int spawn_background_ex(int argc, char **argv, int in_fd, int out_fd, int err_fd, int is_pipe, int quiet_start,
                               int max_heap_kb, uint32 max_runtime_ms, int pgid_hint, int *out_job_id)
{
  int i;
  int slot = -1;
  int id = 0;
  int ppid = 1;
  int sid = 1;
  int pgid = 0;
  int pos = 0;
  int assigned_core = 0;
  TaskHandle_t handle = 0;
  ksh_job_task_t *t = 0;

  if(argc <= 0 || argc >= KSH_MAX_ARGS || argv == 0 || argv[0] == 0){
    puts_line("jobs: invalid arguments");
    return -1;
  }

  ppid = (int)k_getpid();
  sid = (int)k_getsid((pid_t)0);
  if(ppid <= 0)
    ppid = 1;

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  for(i = 0; i < KSH_MAX_JOBS; i++){
    if(!g_jobs[i].used){
      slot = i;
      break;
    }
  }
  if(slot < 0){
    reap_finished_jobs_locked();
    for(i = 0; i < KSH_MAX_JOBS; i++){
      if(!g_jobs[i].used){
        slot = i;
        break;
      }
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
  t->in_fd = (in_fd < 3) ? in_fd : -1;
  t->out_fd = (out_fd < 3) ? out_fd : -1;
  t->err_fd = (err_fd < 3) ? err_fd : -1;
  t->max_heap_kb = max_heap_kb;
  if(xv6_getcwd(t->cwd, sizeof(t->cwd)) != 0)
    k_copy_cstr(t->cwd, sizeof(t->cwd), "/");

  if(in_fd >= 3){
    t->in_fd = hostabi_posix_fs_dup(in_fd);
    if(t->in_fd < 0){
      free_job_ctx(t);
      if(g_jobs_lock)
        (void)xSemaphoreGive(g_jobs_lock);
      puts_line("jobs: fd dup failed");
      return -1;
    }
  }
  if(out_fd >= 3){
    int dupfd = hostabi_posix_fs_dup(out_fd);
    if(dupfd < 0){
      free_job_ctx(t);
      if(g_jobs_lock)
        (void)xSemaphoreGive(g_jobs_lock);
      puts_line("jobs: fd dup failed");
      return -1;
    }
    t->out_fd = dupfd;
  }
  if(err_fd >= 3){
    int dupfd = hostabi_posix_fs_dup(err_fd);
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
      t->argc = i;
      free_job_ctx(t);
      if(g_jobs_lock)
        (void)xSemaphoreGive(g_jobs_lock);
      puts_line("jobs: no memory");
      return -1;
    }
    memcpy(t->argv[i], argv[i], n);
  }
  t->argv[argc] = 0;

  id = alloc_job_id_locked();
  if(id < 1){
    free_job_ctx(t);
    if(g_jobs_lock)
      (void)xSemaphoreGive(g_jobs_lock);
    puts_line("jobs: id alloc failed");
    return -1;
  }

  if(sid <= 0)
    sid = id;
  pgid = (pgid_hint > 0) ? pgid_hint : id;
  if(ppid <= 0)
    ppid = 1;

  assigned_core = select_job_core_locked();

  memset(&g_jobs[slot], 0, sizeof(g_jobs[slot]));
  g_jobs[slot].used = 1;
  g_jobs[slot].id = id;
  g_jobs[slot].pid = id;
  g_jobs[slot].ppid = ppid;
  g_jobs[slot].pgid = pgid;
  g_jobs[slot].sid = sid;
  g_jobs[slot].user_visible = quiet_start ? 0 : 1;
  g_jobs[slot].is_pipe = is_pipe;
  g_jobs[slot].stopped = 0;
  g_jobs[slot].wait_pending_exit = 0;
  g_jobs[slot].wait_pending_stop = 0;
  g_jobs[slot].wait_pending_cont = 0;
  g_jobs[slot].max_heap_kb = max_heap_kb;
  g_jobs[slot].max_runtime_ms = max_runtime_ms;
  g_jobs[slot].started_ms = (uint32)k_ticks();
  g_jobs[slot].reason = JOB_REASON_NONE;
  g_jobs[slot].assigned_core = assigned_core;
  g_jobs[slot].running_core = -1;
  g_jobs[slot].task_ctx = t;

  t->pid = id;
  t->ppid = ppid;
  t->pgid = pgid;
  t->sid = sid;

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

  if((runtime_core_count() > 1
          ? xTaskCreatePinnedToCore(job_task, "xv6_bg", KSH_BG_STACK, t, 5, &handle, assigned_core)
          : xTaskCreate(job_task, "xv6_bg", KSH_BG_STACK, t, 5, &handle)) != pdPASS){
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
  if(g_jobs[slot].used && !g_jobs[slot].done && g_jobs[slot].task_ctx == t)
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
                                      uint32 max_runtime_ms, int pgid_hint)
{
  int rc;
  int job_id = -1;
  int exit_code = 127;
  int allow_ctrl_c = shell_allow_ctrl_c_poll();
  int fg_pgid = -1;

  /*
   * duktape_demo has its own interactive Ctrl+C handling in user space.
   * Keep shell-side force-kill disabled here to avoid races that can
   * terminate the task mid-runtime cleanup.
   */
  if(argc > 0 && argv && argv[0]){
    const char *cmd = argv[0];
    const char *base = strrchr(cmd, '/');
    if(base)
      cmd = base + 1;
    if(strcmp(cmd, "duktape_demo") == 0)
      allow_ctrl_c = 0;
  }

  rc = spawn_background_ex(argc, argv, in_fd, out_fd, err_fd, 0, 1, max_heap_kb, max_runtime_ms, pgid_hint, &job_id);
  if(rc != 0)
    return -1;

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  {
    int slot = job_find_slot_by_id(job_id);
    if(slot >= 0 && g_jobs[slot].used)
      fg_pgid = g_jobs[slot].pgid;
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);
  if(fg_pgid > 0)
    (void)k_tcsetpgrp(0, (pid_t)fg_pgid);

  rc = wait_job_id_ex(job_id, &exit_code, 1, allow_ctrl_c);
  (void)k_tcsetpgrp(0, (pid_t)g_shell_pgid);
  if(rc != 0)
    return -1;
  if(exit_code == 124){
    puts_line("limit: timeout");
    return 124;
  }
  if(exit_code == (128 + SIGTSTP))
    return exit_code & 0xff;
  return exit_code;
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
      (void)hostabi_posix_fs_close(vals[i]);
  }
}

static int parse_exec_and_redir(int argc, char **argv, const ksh_io_t *base_io, char **exec_argv, int max_exec,
                                int *out_argc, ksh_io_t *out_io, int *out_status)
{
  int i;
  int n = 0;
  ksh_io_t io;

  if(base_io == 0 || exec_argv == 0 || out_argc == 0 || out_io == 0)
    return -1;
  if(out_status)
    *out_status = 1;

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
        if(out_status)
          *out_status = 2;
        goto fail;
      }
      if(n >= max_exec - 1){
        puts_line("exec: too many args");
        if(out_status)
          *out_status = 2;
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
      if(out_status)
        *out_status = 2;
      goto fail;
    }

    if(i + 1 >= argc){
      puts_line("redir: missing path");
      if(out_status)
        *out_status = 2;
      goto fail;
    }
    path = argv[++i];
    if(is_control_token(path)){
      puts_line("redir: bad path");
      if(out_status)
        *out_status = 2;
      goto fail;
    }

    if(op[0] == '<')
      flags = O_RDONLY;
    else if(strcmp(op, ">>") == 0)
      flags = O_WRONLY | O_CREAT | O_APPEND;
    else
      flags = O_WRONLY | O_CREAT | O_TRUNC;

    fd = hostabi_posix_fs_open_mode(path, flags, 0666);
    if(fd < 0){
      puts_console("redir: open failed: ");
      puts_line(path);
      if(out_status)
        *out_status = 1;
      goto fail;
    }

    if(target_fd == 0)
      dst = &io.in_fd;
    else if(target_fd == 1)
      dst = &io.out_fd;
    else if(target_fd == 2)
      dst = &io.err_fd;
    else {
      (void)hostabi_posix_fs_close(fd);
      puts_line("redir: bad fd");
      if(out_status)
        *out_status = 2;
      goto fail;
    }

    if(*dst >= 3 && *dst != base_io->in_fd && *dst != base_io->out_fd && *dst != base_io->err_fd)
      (void)hostabi_posix_fs_close(*dst);
    *dst = fd;
  }

  if(n <= 0){
    puts_line("syntax: empty command");
    if(out_status)
      *out_status = 2;
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
  int fail_rc = 1;
  int pipeline_pgid = 0;

  for(i = 0; i < KSH_MAX_STAGES - 1; i++){
    pipe_r[i] = -1;
    pipe_w[i] = -1;
  }

  stage_count = build_pipeline(argc, argv, stage_starts, stage_lens, KSH_MAX_STAGES);
  if(stage_count < 2){
    puts_line("pipe: syntax");
    return 2;
  }

  for(i = 0; i < stage_count - 1; i++){
    int fds[2];
    if(hostabi_posix_fs_pipe(fds) != 0){
      puts_line("pipe: alloc failed");
      fail_rc = 1;
      goto fail;
    }
    pipe_r[i] = fds[0];
    pipe_w[i] = fds[1];
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
    int parse_status = 1;

    base_io.in_fd = base_in;
    base_io.out_fd = base_out;
    base_io.err_fd = 2;
    if(parse_exec_and_redir(stage_argc, stage_argv, &base_io, stage_exec, KSH_MAX_ARGS, &stage_exec_argc, &io,
                            &parse_status) != 0){
      fail_rc = parse_status;
      goto fail;
    }

    if(i > 0 && io.in_fd != base_in && pipe_r[i - 1] >= 3){
      (void)hostabi_posix_fs_close(pipe_r[i - 1]);
      pipe_r[i - 1] = -1;
    }
    if(i < stage_count - 1 && io.out_fd != base_out && pipe_w[i] >= 3){
      (void)hostabi_posix_fs_close(pipe_w[i]);
      pipe_w[i] = -1;
    }

    if(i == stage_count - 1 && !run_bg){
      final_rc = run_foreground_with_limits(stage_exec_argc, stage_exec, io.in_fd, io.out_fd, io.err_fd, max_heap_kb,
                                            max_runtime_ms, pipeline_pgid);
      if(final_rc < 0){
        close_io_custom_fds(&io, &base_io);
        fail_rc = 1;
        goto fail;
      }
    } else {
      if(spawn_background_ex(stage_exec_argc, stage_exec, io.in_fd, io.out_fd, io.err_fd, 1, run_bg ? 0 : 1, max_heap_kb,
                             max_runtime_ms, pipeline_pgid, &sid) != 0)
      {
        close_io_custom_fds(&io, &base_io);
        fail_rc = 1;
        goto fail;
      }
      if(pipeline_pgid == 0)
        pipeline_pgid = sid;
      jobs[njobs++] = sid;
    }
    close_io_custom_fds(&io, &base_io);

    if(i > 0 && pipe_r[i - 1] >= 3){
      (void)hostabi_posix_fs_close(pipe_r[i - 1]);
      pipe_r[i - 1] = -1;
    }
    if(i < stage_count - 1 && pipe_w[i] >= 3){
      (void)hostabi_posix_fs_close(pipe_w[i]);
      pipe_w[i] = -1;
    }
  }

  if(!run_bg){
    if(final_rc == 130){
      cleanup_spawned_jobs(jobs, njobs);
      return 130;
    }
    for(i = 0; i < njobs; i++){
      int stage_rc = 0;
      if(wait_job_id_ex(jobs[i], &stage_rc, 1, shell_allow_ctrl_c_poll()) != 0)
        continue;
      if(stage_rc == 130){
        cleanup_spawned_jobs(jobs + i + 1, njobs - (i + 1));
        return 130;
      }
    }
  }

  return final_rc;

fail:
  cleanup_spawned_jobs(jobs, njobs);
  for(i = 0; i < KSH_MAX_STAGES - 1; i++){
    if(pipe_r[i] >= 3)
      (void)hostabi_posix_fs_close(pipe_r[i]);
    if(pipe_w[i] >= 3)
      (void)hostabi_posix_fs_close(pipe_w[i]);
  }
  return fail_rc;
}

static int execute_external(int argc, char **argv, int run_bg, int max_heap_kb, uint32 max_runtime_ms)
{
  char *exec_argv[KSH_MAX_ARGS];
  int exec_argc = 0;
  int rc;
  int parse_status = 1;
  ksh_io_t base_io;
  ksh_io_t io;

  if(find_pipe_pos(argc, argv) >= 0)
    return run_pipeline(argc, argv, run_bg, max_heap_kb, max_runtime_ms);

  base_io.in_fd = 0;
  base_io.out_fd = 1;
  base_io.err_fd = 2;

  if(parse_exec_and_redir(argc, argv, &base_io, exec_argv, KSH_MAX_ARGS, &exec_argc, &io, &parse_status) != 0)
    return parse_status;

  if(run_bg)
    rc = spawn_background_ex(exec_argc, exec_argv, io.in_fd, io.out_fd, io.err_fd, 0, 0, max_heap_kb, max_runtime_ms, 0,
                             0);
  else
    rc = run_foreground_with_limits(exec_argc, exec_argv, io.in_fd, io.out_fd, io.err_fd, max_heap_kb, max_runtime_ms, 0);

  close_io_custom_fds(&io, &base_io);
  if(rc < 0)
    return 1;
  return rc & 0xff;
}

static int cmd_jobs(void)
{
  int i;
  int any = 0;

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  for(i = 0; i < KSH_MAX_JOBS; i++){
    if(!g_jobs[i].used || !g_jobs[i].user_visible)
      continue;
    if(g_jobs[i].done)
      k_printf("[%d] done %s\r\n", g_jobs[i].id, g_jobs[i].cmd);
    else if(g_jobs[i].stopped)
      k_printf("[%d] stopped %s\r\n", g_jobs[i].id, g_jobs[i].cmd);
    else
      k_printf("[%d] running %s\r\n", g_jobs[i].id, g_jobs[i].cmd);
    any = 1;
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(!any)
    puts_line("jobs: empty");
  return 0;
}

static int cmd_ps(void)
{
  int i;
  int any = 0;
  int shell_core = current_core_id();

  k_printf("PID STATE EXIT REASON CORE(A/R) LIMIT(ms/kb) CMD\r\n");
  k_printf("0 RUN - - %d/%d - sh\r\n", shell_core, shell_core);

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  for(i = 0; i < KSH_MAX_JOBS; i++){
    if(!g_jobs[i].used || !g_jobs[i].user_visible)
      continue;
    if(g_jobs[i].done){
      k_printf("%d DONE %d %s %d/%d %u/%d %s\r\n", g_jobs[i].id, g_jobs[i].exit_code, job_reason_str(g_jobs[i].reason),
               g_jobs[i].assigned_core, g_jobs[i].running_core, (unsigned)g_jobs[i].max_runtime_ms,
               g_jobs[i].max_heap_kb, g_jobs[i].cmd);
    } else if(g_jobs[i].stopped){
      k_printf("%d STOP - %s %d/%d %u/%d %s\r\n", g_jobs[i].id, job_reason_str(g_jobs[i].reason), g_jobs[i].assigned_core,
               g_jobs[i].running_core, (unsigned)g_jobs[i].max_runtime_ms, g_jobs[i].max_heap_kb, g_jobs[i].cmd);
    } else {
      k_printf("%d RUN - - %d/%d %u/%d %s\r\n", g_jobs[i].id, g_jobs[i].assigned_core, g_jobs[i].running_core,
               (unsigned)g_jobs[i].max_runtime_ms, g_jobs[i].max_heap_kb, g_jobs[i].cmd);
    }
    any = 1;
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(!any)
    k_printf("- NOJOBS -\r\n");
  return 0;
}

static int cmd_health(void)
{
  int i;
  int jobs_used = 0;
  int jobs_running = 0;
  int jobs_done = 0;
  int jobs_timeout = 0;
  int jobs_killed = 0;
  int core_count = runtime_core_count();
  int core_running[KSH_MAX_CORES];
  int core_spread = 0;
  char core_load[96];
  int core_pos = 0;
  uint32 max_running_job_ms = 0;
  uint32 now = (uint32)k_ticks();
  uint64 free_heap = hal_free_heap_bytes();
  uint64 uptime_ms = k_ticks_to_ms_u64((uint64)now);

  if(core_count < 1)
    core_count = 1;
  if(core_count > KSH_MAX_CORES)
    core_count = KSH_MAX_CORES;
  memset(core_running, 0, sizeof(core_running));

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  for(i = 0; i < KSH_MAX_JOBS; i++){
    uint32 elapsed_ms;
    if(!g_jobs[i].used || !g_jobs[i].user_visible)
      continue;
    jobs_used++;
    if(g_jobs[i].done){
      jobs_done++;
      if(g_jobs[i].reason == JOB_REASON_TIMEOUT)
        jobs_timeout++;
      else if(g_jobs[i].reason == JOB_REASON_KILLED)
        jobs_killed++;
      continue;
    }

    jobs_running++;
    if(g_jobs[i].assigned_core >= 0 && g_jobs[i].assigned_core < core_count)
      core_running[g_jobs[i].assigned_core]++;
    elapsed_ms = k_ticks_to_ms_u32((uint32)(now - g_jobs[i].started_ms));
    if(elapsed_ms > max_running_job_ms)
      max_running_job_ms = elapsed_ms;
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(core_count > 0){
    int min_jobs = core_running[0];
    int max_jobs = core_running[0];
    for(i = 1; i < core_count; i++){
      if(core_running[i] < min_jobs)
        min_jobs = core_running[i];
      if(core_running[i] > max_jobs)
        max_jobs = core_running[i];
    }
    core_spread = max_jobs - min_jobs;
  }

  core_load[0] = 0;
  for(i = 0; i < core_count; i++){
    int n = snprintf(core_load + core_pos, sizeof(core_load) - (unsigned)core_pos, "%s%d:%d", (i ? "," : ""), i,
                     core_running[i]);
    if(n <= 0 || core_pos + n >= (int)sizeof(core_load)){
      core_load[sizeof(core_load) - 1] = 0;
      break;
    }
    core_pos += n;
  }

  k_printf(
      "health: uptime_ms=%llu free_heap_bytes=%llu jobs_used=%d jobs_running=%d jobs_done=%d jobs_timeout=%d jobs_killed=%d "
      "max_running_job_ms=%u core_load=%s core_spread=%d ulimit_ms=%u ulimit_heap_kb=%d\r\n",
      (unsigned long long)uptime_ms, (unsigned long long)free_heap, jobs_used, jobs_running, jobs_done, jobs_timeout,
      jobs_killed,
      (unsigned)max_running_job_ms, core_load, core_spread, (unsigned)g_ulimit_ms, g_ulimit_heap_kb);
  return 0;
}

static int cmd_wait(int argc, char **argv)
{
  if(argc == 1){
    while(1){
      int i;
      int has_running = 0;
      if(g_jobs_lock)
        (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
      for(i = 0; i < KSH_MAX_JOBS; i++){
        if(!g_jobs[i].used)
          continue;
        if(g_jobs[i].done)
          memset(&g_jobs[i], 0, sizeof(g_jobs[i]));
        else if(g_jobs[i].user_visible && !g_jobs[i].stopped)
          has_running = 1;
      }
      if(g_jobs_lock)
        (void)xSemaphoreGive(g_jobs_lock);
      if(!has_running)
        break;
      if(shell_allow_ctrl_c_poll() && hostabi_posix_tty_poll_signal() == SIGINT){
        puts_line("^C");
        return 130;
      }
      enforce_job_limits();
      hal_delay_ms(10);
    }
    puts_line("wait: done");
    return 0;
  }

  if(argc == 2){
    uint32 id = 0;
    int exit_code = 0;
    if(parse_u32_dec(argv[1], &id) != 0){
      puts_line("wait: bad job id");
      return 2;
    }
    if(!job_is_user_visible_id((int)id)){
      puts_line("wait: no such job");
      return 1;
    }
    if(wait_job_id_ex((int)id, &exit_code, 1, shell_allow_ctrl_c_poll()) != 0){
      puts_line("wait: no such job");
      return 1;
    }
    k_printf("wait: done %u\r\n", (unsigned)exit_code);
    return exit_code & 0xff;
  }

  puts_line("usage: wait [jobid]");
  return 2;
}

static int cmd_kill(int argc, char **argv)
{
  uint32 id = 0;

  if(argc != 2 || parse_u32_dec(argv[1], &id) != 0){
    puts_line("usage: kill <jobid>");
    return 2;
  }

  if(!job_is_user_visible_id((int)id)){
    puts_line("kill: no such job");
    return 1;
  }
  if(terminate_job_id((int)id, 137, JOB_REASON_KILLED) != 0){
    puts_line("kill: no such job");
    return 1;
  }

  puts_line("kill: ok");
  return 0;
}

static int cmd_fg(int argc, char **argv)
{
  uint32 id = 0;
  int exit_code = 0;
  int pgid = -1;
  int was_stopped = 0;

  if(argc != 2 || parse_u32_dec(argv[1], &id) != 0){
    puts_line("usage: fg <jobid>");
    return 2;
  }

  if(!job_is_user_visible_id((int)id)){
    puts_line("fg: no such job");
    return 1;
  }

  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  {
    int slot = job_find_slot_by_id((int)id);
    if(slot >= 0 && g_jobs[slot].used){
      pgid = g_jobs[slot].pgid;
      was_stopped = g_jobs[slot].stopped;
    }
  }
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);

  if(was_stopped)
    (void)resume_job_id((int)id);
  if(pgid > 0)
    (void)k_tcsetpgrp(0, (pid_t)pgid);

  if(wait_job_id_ex((int)id, &exit_code, 1, shell_allow_ctrl_c_poll()) != 0){
    (void)k_tcsetpgrp(0, (pid_t)g_shell_pgid);
    puts_line("fg: no such job");
    return 1;
  }

  (void)k_tcsetpgrp(0, (pid_t)g_shell_pgid);
  if(exit_code == (128 + SIGTSTP)){
    k_printf("fg: stopped %u\r\n", (unsigned)id);
    return exit_code & 0xff;
  }

  k_printf("fg: done %u\r\n", (unsigned)exit_code);
  return exit_code & 0xff;
}

static int cmd_bg(int argc, char **argv)
{
  uint32 id = 0;

  if(argc != 2 || parse_u32_dec(argv[1], &id) != 0){
    puts_line("usage: bg <jobid>");
    return 2;
  }
  if(!job_is_user_visible_id((int)id)){
    puts_line("bg: no such job");
    return 1;
  }
  if(resume_job_id((int)id) != 0){
    puts_line("bg: no such stopped job");
    return 1;
  }

  k_printf("bg: continued %u\r\n", (unsigned)id);
  return 0;
}

static int cmd_ulimit(int argc, char **argv)
{
  uint32 v = 0;

  if(argc == 1){
    k_printf("ulimit: -t %u ms, -m %d kb\r\n", (unsigned)g_ulimit_ms, g_ulimit_heap_kb);
    return 0;
  }

  if(argc == 2){
    if(strcmp(argv[1], "-t") == 0){
      k_printf("%u\r\n", (unsigned)g_ulimit_ms);
      return 0;
    }
    if(strcmp(argv[1], "-m") == 0){
      k_printf("%d\r\n", g_ulimit_heap_kb);
      return 0;
    }
    puts_line("usage: ulimit [-t ms] [-m kb]");
    return 2;
  }

  if(argc == 3){
    if(parse_u32_dec(argv[2], &v) != 0){
      puts_line("ulimit: bad value");
      return 2;
    }
    if(strcmp(argv[1], "-t") == 0){
      g_ulimit_ms = v;
      return 0;
    }
    if(strcmp(argv[1], "-m") == 0){
      g_ulimit_heap_kb = (int)v;
      return 0;
    }
  }

  puts_line("usage: ulimit [-t ms] [-m kb]");
  return 2;
}

static int cmd_limit(int argc, char **argv, int run_bg)
{
  uint32 max_ms = 0;
  uint32 max_kb = 0;

  if(argc < 4){
    puts_line("usage: limit <ms> <heap_kb> <cmd...>");
    return 2;
  }
  if(parse_u32_dec(argv[1], &max_ms) != 0 || parse_u32_dec(argv[2], &max_kb) != 0){
    puts_line("limit: bad numeric args");
    return 2;
  }

  return execute_external(argc - 3, argv + 3, run_bg, (int)max_kb, max_ms);
}

static int cmd_time(int argc, char **argv, int run_bg)
{
  uint32 start;
  uint32 end;
  int rc;
  if(argc < 2){
    puts_line("usage: time <cmd...>");
    return 2;
  }

  start = (uint32)k_ticks();
  rc = dispatch_command(argc - 1, argv + 1, run_bg);
  end = (uint32)k_ticks();

  if(!run_bg)
    k_eprintf("time: %u ms\r\n", (unsigned)k_ticks_to_ms_u32((uint32)(end - start)));
  return rc;
}

static int cmd_pwd(void)
{
  char cwd[MAXPATH];
  if(xv6_getcwd(cwd, sizeof(cwd)) != 0){
    eputs_line("pwd: failed");
    return 1;
  }
  puts_line(cwd);
  return 0;
}

static int cmd_cd(int argc, char **argv)
{
  const char *path = 0;

  if(argc > 2){
    eputs_line("usage: cd [dir]");
    return 2;
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
    return 1;
  }
  env_sync_pwd();
  return 0;
}

static int cmd_env(void)
{
  int i;
  for(i = 0; i < KSH_MAX_ENV; i++){
    if(!g_env[i].used)
      continue;
    k_printf("%s=%s\r\n", g_env[i].key, g_env[i].val);
  }
  return 0;
}

static int cmd_export(int argc, char **argv)
{
  int i;
  int rc = 0;

  if(argc == 1){
    return cmd_env();
  }
  for(i = 1; i < argc; i++){
    char *eq = strchr(argv[i], '=');
    if(eq){
      char key[KSH_ENV_KEY];
      int n = (int)(eq - argv[i]);
      if(n <= 0 || n >= (int)sizeof(key)){
        eputs_console("export: bad name: ");
        eputs_line(argv[i]);
        rc = 1;
        continue;
      }
      memcpy(key, argv[i], (unsigned)n);
      key[n] = 0;
      if(env_set(key, eq + 1) != 0){
        eputs_console("export: bad assignment: ");
        eputs_line(argv[i]);
        rc = 1;
      }
    } else {
      if(env_set(argv[i], "") != 0){
        eputs_console("export: bad name: ");
        eputs_line(argv[i]);
        rc = 1;
      }
    }
  }
  return rc;
}

static int cmd_unset(int argc, char **argv)
{
  int i;
  if(argc < 2){
    eputs_line("usage: unset NAME...");
    return 2;
  }
  for(i = 1; i < argc; i++){
    if(strcmp(argv[i], "PWD") == 0)
      continue;
    env_unset(argv[i]);
  }
  return 0;
}

static int cmd_source(int argc, char **argv)
{
  FILE *f;
  char line[256];

  if(argc != 2){
    eputs_line("usage: . <file>");
    return 2;
  }

  f = fopen(argv[1], "r");
  if(f == 0){
    eputs_console(".: cannot open: ");
    eputs_line(argv[1]);
    return 1;
  }

  while(fgets(line, sizeof(line), f) != 0){
    int exit_code = 0;
    if(eval_line_inner(line, &exit_code) != 0){
      fclose(f);
      return exit_code & 0xff;
    }
    if(exit_code != 0){
      fclose(f);
      return exit_code & 0xff;
    }
  }

  fclose(f);
  return 0;
}

static int cmd_kmod(int argc, char **argv)
{
  if(argc < 2){
    puts_line("usage: kmod <list|load|unload|reload|autoload|verify> ...");
    return 2;
  }

  if(strcmp(argv[1], "list") == 0){
    kmod_info_t mods[16];
    int i;
    int n = 0;
    if(kmod_list(mods, (int)(sizeof(mods) / sizeof(mods[0])), &n) != 0){
      eputs_console("kmod list: ");
      eputs_line(kmod_last_error());
      return 1;
    }
    if(n == 0){
      puts_line("kmod: empty");
      return 0;
    }
    k_printf("ID PRI REF SIG NAME PATH\r\n");
    for(i = 0; i < n && i < (int)(sizeof(mods) / sizeof(mods[0])); i++){
      k_printf("%d %d %d %s %s %s\r\n", mods[i].module_id, mods[i].priority, mods[i].refcnt,
               mods[i].signed_ok ? "yes" : "no", mods[i].name, mods[i].path);
    }
    return 0;
  }

  if(strcmp(argv[1], "load") == 0){
    int id = -1;
    int prio = KMOD_PRIORITY_AUTO;
    if(argc < 3){
      puts_line("usage: kmod load <path> [priority]");
      return 2;
    }
    if(argc >= 4){
      if(parse_i32_dec(argv[3], &prio) != 0){
        eputs_line("kmod load: bad priority");
        return 2;
      }
    }
    if(kmod_load_with_priority(argv[2], prio, &id) != 0){
      eputs_console("kmod load: ");
      eputs_line(kmod_last_error());
      return 1;
    }
    k_printf("kmod: loaded id=%d\r\n", id);
    return 0;
  }

  if(strcmp(argv[1], "unload") == 0){
    uint32 id_u32 = 0;
    int id;
    int force = 0;
    if(argc < 3){
      puts_line("usage: kmod unload <id> [--force]");
      return 2;
    }
    if(parse_u32_dec(argv[2], &id_u32) != 0 || id_u32 == 0 || id_u32 > 2147483647u){
      eputs_line("kmod unload: bad id");
      return 2;
    }
    id = (int)id_u32;
    if(argc >= 4){
      if(strcmp(argv[3], "--force") != 0){
        puts_line("usage: kmod unload <id> [--force]");
        return 2;
      }
      force = 1;
    }
    if(argc > 4){
      puts_line("usage: kmod unload <id> [--force]");
      return 2;
    }
    if(kmod_unload(id, force) != 0){
      eputs_console("kmod unload: ");
      eputs_line(kmod_last_error());
      return 1;
    }
    if(strstr(kmod_last_error(), "decremented refcnt"))
      k_printf("kmod: decremented refcnt id=%d\r\n", id);
    else
      k_printf("kmod: unloaded id=%d%s\r\n", id, force ? " (force)" : "");
    return 0;
  }

  if(strcmp(argv[1], "reload") == 0){
    uint32 id_u32 = 0;
    int id = 0;
    int new_id = -1;
    if(argc < 3){
      puts_line("usage: kmod reload <id|path> [priority]");
      return 2;
    }

    if(parse_u32_dec(argv[2], &id_u32) == 0 && id_u32 <= 2147483647u)
      id = (int)id_u32;
    if(id > 0){
      if(kmod_reload(id, &new_id) != 0){
        eputs_console("kmod reload: ");
        eputs_line(kmod_last_error());
        return 1;
      }
      k_printf("kmod: reloaded id=%d -> id=%d\r\n", id, new_id);
      return 0;
    }

    {
      int prio = KMOD_PRIORITY_AUTO;
      if(argc >= 4){
        if(parse_i32_dec(argv[3], &prio) != 0){
          eputs_line("kmod reload: bad priority");
          return 2;
        }
      }
      if(kmod_reload_path(argv[2], prio, &new_id) != 0){
        eputs_console("kmod reload: ");
        eputs_line(kmod_last_error());
        return 1;
      }
      k_printf("kmod: reloaded path id=%d\r\n", new_id);
      return 0;
    }
  }

  if(strcmp(argv[1], "autoload") == 0){
    int loaded = 0;
    const char *manifest = (argc >= 3) ? argv[2] : 0;
    if(kmod_autoload_from_manifest(manifest, &loaded) != 0){
      eputs_console("kmod autoload: ");
      eputs_line(kmod_last_error());
      return 1;
    }
    k_printf("kmod: autoload loaded=%d\r\n", loaded);
    return 0;
  }

  if(strcmp(argv[1], "verify") == 0){
    int signed_ok = 0;
    if(argc < 3){
      puts_line("usage: kmod verify <path>");
      return 2;
    }
    if(kmod_verify_signature(argv[2], &signed_ok) != 0){
      eputs_console("kmod verify: ");
      eputs_line(kmod_last_error());
      return 1;
    }
    k_printf("kmod: %s\r\n", signed_ok ? "signed" : "unsigned");
    return 0;
  }

  puts_line("usage: kmod <list|load|unload|reload|autoload|verify> ...");
  return 2;
}

static int register_default_symbols(void)
{
  static const elf_host_symbol_t syms[] = {
    { "puts", (void *)k_host_puts },
    { "printf", (void *)k_host_printf },
    { "fprintf", (void *)k_host_fprintf },
    { "vfprintf", (void *)k_host_vfprintf },
    { "sprintf", (void *)sprintf },
    { "snprintf", (void *)snprintf },
    { "vsprintf", (void *)vsprintf },
    { "vsnprintf", (void *)vsnprintf },
    { "fputs", (void *)k_fputs },
    { "fputc", (void *)k_fputc },
    { "fopen", (void *)k_fopen },
    { "__xv6_host_fopen", (void *)k_fopen },
    { "freopen", (void *)k_freopen },
    { "fclose", (void *)k_fclose },
    { "__xv6_host_fclose", (void *)k_fclose },
    { "fgets", (void *)k_fgets },
    { "fgetc", (void *)k_fgetc },
    { "getc", (void *)k_getc },
    { "getchar", (void *)k_getchar },
    { "fread", (void *)k_fread },
    { "fwrite", (void *)k_fwrite },
    { "setbuf", (void *)k_setbuf },
    { "setvbuf", (void *)k_setvbuf },
    { "putchar", (void *)putchar },
    { "putc", (void *)k_putc },
    { "fflush", (void *)k_fflush },
    { "clearerr", (void *)k_clearerr },
    { "feof", (void *)k_feof },
    { "ferror", (void *)k_ferror },
    { "fseek", (void *)k_fseek },
    { "ftell", (void *)k_ftell },
    { "rewind", (void *)k_rewind },
    { "ungetc", (void *)k_ungetc },
    { "perror", (void *)perror },
    { "strerror", (void *)strerror },
    { "fileno", (void *)k_fileno },
    { "abort", (void *)k_abort },
    { "malloc", (void *)malloc },
    { "calloc", (void *)calloc },
    { "realloc", (void *)realloc },
    { "free", (void *)free },
    { "memset", (void *)memset },
    { "memcpy", (void *)k_host_memcpy },
    { "bcmp", (void *)k_bcmp },
    { "bcopy", (void *)k_bcopy },
    { "bzero", (void *)k_bzero },
    { "index", (void *)strchr },
    { "rindex", (void *)strrchr },
    { "strlen", (void *)k_host_strlen },
    { "strcmp", (void *)k_host_strcmp },
    { "usleep", (void *)usleep },
    { "k_ticks", (void *)k_ticks },
    { "k_puts", (void *)k_puts },
    { "k_free_heap", (void *)k_free_heap },
    { "k_total_heap", (void *)k_total_heap },
    { "xv6fs_readdir_path", (void *)k_fs_readdir_path },
    { "xv6fs_read_file_alloc_path", (void *)xv6fs_read_file_alloc_path },
    { "xv6fs_write_file_path", (void *)xv6fs_write_file_path },
    { "xv6fs_mkdir_path", (void *)xv6fs_mkdir_path },
    { "xv6fs_unlink_path", (void *)xv6fs_unlink_path },
    { "xv6fs_rmdir_path", (void *)xv6fs_rmdir_path },
    { "xv6fs_rename_path", (void *)xv6fs_rename_path },
    { "xv6_open", (void *)xv6_open },
    { "xv6_dup", (void *)xv6_dup },
    { "xv6_read", (void *)xv6_read },
    { "xv6_write", (void *)xv6_write },
    { "xv6_close", (void *)xv6_close },
    { "xv6_chdir", (void *)xv6_chdir },
    { "xv6_getcwd", (void *)xv6_getcwd },
    { "xv6_ptsname", (void *)xv6_ptsname },
    { "xv6_pipe", (void *)xv6_pipe },
    { "pipe", (void *)hostabi_posix_fs_pipe },
    { "__xv6_host_pipe", (void *)hostabi_posix_fs_pipe },
    { "open", (void *)k_open },
    { "creat", (void *)k_creat },
    { "read", (void *)k_read },
    { "write", (void *)k_write },
    { "close", (void *)k_close },
    { "__xv6_host_close", (void *)k_close },
    { "__xv6_posix_close", (void *)k_posix_close },
    { "dup", (void *)k_dup },
    { "dup2", (void *)k_dup2 },
    { "lseek", (void *)k_lseek },
    { "fcntl", (void *)hostabi_posix_fcntl },
    { "__xv6_host_fcntl", (void *)hostabi_posix_fcntl },
    { "__xv6_posix_fcntl", (void *)k_posix_fcntl },
    { "ioctl", (void *)hostabi_posix_ioctl },
    { "__xv6_host_ioctl", (void *)hostabi_posix_ioctl },
    { "__xv6_posix_ioctl", (void *)k_posix_ioctl },
    { "socket", (void *)k_socket },
    { "__xv6_posix_socket", (void *)k_posix_socket },
    { "bind", (void *)k_bind },
    { "__xv6_posix_bind", (void *)k_posix_bind },
    { "listen", (void *)k_listen },
    { "__xv6_posix_listen", (void *)k_posix_listen },
    { "accept", (void *)k_accept },
    { "__xv6_posix_accept", (void *)k_posix_accept },
    { "connect", (void *)k_connect },
    { "__xv6_posix_connect", (void *)k_posix_connect },
    { "send", (void *)k_send },
    { "__xv6_posix_send", (void *)k_posix_send },
    { "recv", (void *)k_recv },
    { "__xv6_posix_recv", (void *)k_posix_recv },
    { "sendto", (void *)k_sendto },
    { "__xv6_posix_sendto", (void *)k_posix_sendto },
    { "recvfrom", (void *)k_recvfrom },
    { "__xv6_posix_recvfrom", (void *)k_posix_recvfrom },
    { "poll", (void *)k_poll },
    { "__xv6_posix_poll", (void *)k_posix_poll },
    { "select", (void *)k_select },
    { "__xv6_posix_select", (void *)k_posix_select },
    { "shutdown", (void *)k_shutdown },
    { "__xv6_posix_shutdown", (void *)k_posix_shutdown },
    { "getsockopt", (void *)k_getsockopt },
    { "__xv6_posix_getsockopt", (void *)k_posix_getsockopt },
    { "setsockopt", (void *)k_setsockopt },
    { "__xv6_posix_setsockopt", (void *)k_posix_setsockopt },
    { "getsockname", (void *)k_getsockname },
    { "__xv6_posix_getsockname", (void *)k_posix_getsockname },
    { "getpeername", (void *)k_getpeername },
    { "__xv6_posix_getpeername", (void *)k_posix_getpeername },
    { "xv6_wifi_host_init_default", (void *)k_wifi_host_init_default },
    { "esp_netif_create_default_wifi_sta", (void *)esp_netif_create_default_wifi_sta },
    { "esp_netif_destroy", (void *)esp_netif_destroy },
    /* Event bases are data symbols; modules expect their addresses for relocations. */
    { "WIFI_EVENT", (void *)&WIFI_EVENT },
    { "IP_EVENT", (void *)&IP_EVENT },
    { "esp_event_handler_instance_register", (void *)esp_event_handler_instance_register },
    { "esp_event_handler_instance_unregister", (void *)esp_event_handler_instance_unregister },
    { "esp_wifi_set_storage", (void *)esp_wifi_set_storage },
    { "esp_wifi_set_mode", (void *)esp_wifi_set_mode },
    { "esp_wifi_start", (void *)esp_wifi_start },
    { "esp_wifi_stop", (void *)esp_wifi_stop },
    { "esp_wifi_connect", (void *)esp_wifi_connect },
    { "esp_wifi_disconnect", (void *)esp_wifi_disconnect },
    { "esp_wifi_set_config", (void *)esp_wifi_set_config },
    { "esp_wifi_scan_start", (void *)esp_wifi_scan_start },
    { "esp_wifi_scan_get_ap_num", (void *)esp_wifi_scan_get_ap_num },
    { "esp_wifi_scan_get_ap_records", (void *)esp_wifi_scan_get_ap_records },
    { "esp_wifi_sta_get_ap_info", (void *)esp_wifi_sta_get_ap_info },
    { "esp_wifi_deinit", (void *)esp_wifi_deinit },
    { "stat", (void *)k_stat },
    { "__xv6_host_stat", (void *)k_stat },
    { "lstat", (void *)k_lstat },
    { "__xv6_host_lstat", (void *)k_lstat },
    { "fstat", (void *)k_fstat },
    { "__xv6_host_fstat", (void *)k_fstat },
    { "access", (void *)k_access },
    { "mkdir", (void *)k_mkdir },
    { "unlink", (void *)k_unlink },
    { "rmdir", (void *)k_rmdir },
    { "chmod", (void *)k_chmod },
    { "chdir", (void *)k_chdir },
    { "getcwd", (void *)k_getcwd },
    { "isatty", (void *)k_isatty },
    { "getpid", (void *)k_getpid },
    { "__xv6_host_getpid", (void *)k_getpid },
    { "getppid", (void *)k_getppid },
    { "__xv6_host_getppid", (void *)k_getppid },
    { "kill", (void *)k_kill },
    { "__xv6_host_kill", (void *)k_kill },
    { "wait", (void *)k_wait },
    { "__xv6_host_wait", (void *)k_wait },
    { "waitpid", (void *)k_waitpid },
    { "__xv6_host_waitpid", (void *)k_waitpid },
    { "waitid", (void *)k_waitid },
    { "__xv6_host_waitid", (void *)k_waitid },
    { "setpgid", (void *)k_setpgid },
    { "getpgid", (void *)k_getpgid },
    { "getpgrp", (void *)k_getpgrp },
    { "setsid", (void *)k_setsid },
    { "getsid", (void *)k_getsid },
    { "tcsetpgrp", (void *)k_tcsetpgrp },
    { "tcgetpgrp", (void *)k_tcgetpgrp },
    { "tcgetattr", (void *)hostabi_posix_tcgetattr },
    { "tcsetattr", (void *)hostabi_posix_tcsetattr },
    { "cfmakeraw", (void *)hostabi_posix_cfmakeraw },
    { "posix_openpt", (void *)hostabi_posix_openpt },
    { "grantpt", (void *)hostabi_grantpt },
    { "unlockpt", (void *)hostabi_unlockpt },
    { "ptsname", (void *)hostabi_ptsname },
    { "ptsname_r", (void *)hostabi_ptsname_r },
    { "utimes", (void *)k_utimes },
    { "lutimes", (void *)k_lutimes },
    { "umask", (void *)k_umask },
    { "sync", (void *)k_sync },
    { "fsync", (void *)k_fsync },
    { "fdatasync", (void *)k_fdatasync },
    { "ftruncate", (void *)k_ftruncate },
    { "truncate", (void *)k_truncate },
    { "link", (void *)k_link },
    { "rename", (void *)k_rename },
    { "symlink", (void *)k_symlink },
    { "readlink", (void *)k_readlink },
    { "mknod", (void *)k_mknod },
    { "mkfifo", (void *)k_mkfifo },
    { "signal", (void *)k_signal },
    { "__xv6_host_signal", (void *)k_signal },
    { "sigaction", (void *)k_sigaction },
    { "__xv6_host_sigaction", (void *)k_sigaction },
    { "sigprocmask", (void *)k_sigprocmask },
    { "sigemptyset", (void *)k_sigemptyset },
    { "sigfillset", (void *)k_sigfillset },
    { "sigaddset", (void *)k_sigaddset },
    { "sigdelset", (void *)k_sigdelset },
    { "sigismember", (void *)k_sigismember },
    { "raise", (void *)k_raise },
    { "sleep", (void *)k_sleep },
    { "_read_r", (void *)k__read_r },
    { "_write_r", (void *)k__write_r },
    { "_open_r", (void *)k__open_r },
    { "_close_r", (void *)k__close_r },
    { "_lseek_r", (void *)k__lseek_r },
    { "_fstat_r", (void *)k__fstat_r },
    { "_stat_r", (void *)k__stat_r },
    { "_lstat_r", (void *)k__lstat_r },
    { "_isatty_r", (void *)k__isatty_r },
    { "_unlink_r", (void *)k__unlink_r },
    { "_mkdir_r", (void *)k__mkdir_r },
    { "_rmdir_r", (void *)k__rmdir_r },
    { "_rename_r", (void *)k__rename_r },
    { "_link_r", (void *)k__link_r },
    { "_gettimeofday_r", (void *)k__gettimeofday_r },
    { "_times_r", (void *)k__times_r },
    { "_kill_r", (void *)k__kill_r },
    { "_wait_r", (void *)k__wait_r },
    { "_waitpid_r", (void *)k__waitpid_r },
    { "_getpid_r", (void *)k__getpid_r },
    { "_sbrk_r", (void *)k__sbrk_r },
    { "_sbrk", (void *)k__sbrk },
    { "sbrk", (void *)k_sbrk },
    { "gettimeofday", (void *)k_gettimeofday },
    { "__xv6_host_gettimeofday", (void *)k_gettimeofday },
    { "times", (void *)k_times },
    { "__xv6_host_times", (void *)k_times },
    { "time", (void *)k_time },
    { "__xv6_host_time", (void *)k_time },
    { "exit", (void *)k_exit },
    { "_Exit", (void *)k_exit },
    { "_exit", (void *)k__exit },
    { "fchmod", (void *)k_fchmod },
    { "chown", (void *)k_chown },
    { "lchown", (void *)k_lchown },
    { "fchown", (void *)k_fchown },
    { "getopt", (void *)k_getopt },
    { "__xv6_host_getopt", (void *)k_getopt },
    { "opendir", (void *)hostabi_opendir },
    { "readdir", (void *)hostabi_readdir },
    { "closedir", (void *)hostabi_closedir },
    { "rewinddir", (void *)hostabi_rewinddir },
    { "fdopendir", (void *)hostabi_fdopendir },
    { "__xv6_host_opendir", (void *)hostabi_opendir },
    { "__xv6_host_readdir", (void *)hostabi_readdir },
    { "__xv6_host_closedir", (void *)hostabi_closedir },
    { "dlopen", (void *)dlopen },
    { "dlsym", (void *)dlsym },
    { "dlclose", (void *)dlclose },
    { "dlerror", (void *)dlerror },
    { "shrt_eval_line", (void *)shell_runtime_eval_line },
    { "shrt_run_interactive", (void *)shell_runtime_run_interactive },
    { "shrt_reboot", (void *)shell_runtime_reboot },
    { "dirfd", (void *)hostabi_dirfd },
    { "optind", (void *)&k_optind },
    { "opterr", (void *)&k_opterr },
    { "optopt", (void *)&k_optopt },
    { "optarg", (void *)&k_optarg },
    { "optreset", (void *)&k_optreset },
    { "__getreent", (void *)k_getreent },
    { "environ", (void *)&environ },
    { "__environ", (void *)&environ },
  };

  return hostabi_register_exports(syms, (int)(sizeof(syms) / sizeof(syms[0])));
}

static int is_builtin_command(const char *cmd)
{
  return (strcmp(cmd, "help") == 0 || strcmp(cmd, "reboot") == 0 || strcmp(cmd, ".") == 0 || strcmp(cmd, "cd") == 0 ||
          strcmp(cmd, "pwd") == 0 || strcmp(cmd, "env") == 0 || strcmp(cmd, "export") == 0 ||
          strcmp(cmd, "health") == 0 ||
          strcmp(cmd, "unset") == 0 || strcmp(cmd, "ps") == 0 || strcmp(cmd, "jobs") == 0 ||
          strcmp(cmd, "wait") == 0 || strcmp(cmd, "kill") == 0 || strcmp(cmd, "fg") == 0 ||
          strcmp(cmd, "bg") == 0 ||
          strcmp(cmd, "time") == 0 || strcmp(cmd, "ulimit") == 0 || strcmp(cmd, "limit") == 0 ||
          strcmp(cmd, "kmod") == 0);
}

static int dispatch_builtin_command(int argc, char **argv, int run_bg)
{
  if(strcmp(argv[0], "help") == 0){
    return cmd_help();
  }
  if(strcmp(argv[0], "reboot") == 0){
    puts_line("rebooting...");
    hal_reboot();
    return 0;
  }
  if(strcmp(argv[0], ".") == 0){
    return cmd_source(argc, argv);
  }
  if(strcmp(argv[0], "cd") == 0){
    return cmd_cd(argc, argv);
  }
  if(strcmp(argv[0], "pwd") == 0){
    return cmd_pwd();
  }
  if(strcmp(argv[0], "env") == 0){
    return cmd_env();
  }
  if(strcmp(argv[0], "export") == 0){
    return cmd_export(argc, argv);
  }
  if(strcmp(argv[0], "health") == 0){
    return cmd_health();
  }
  if(strcmp(argv[0], "unset") == 0){
    return cmd_unset(argc, argv);
  }
  if(strcmp(argv[0], "ps") == 0){
    return cmd_ps();
  }
  if(strcmp(argv[0], "jobs") == 0){
    return cmd_jobs();
  }
  if(strcmp(argv[0], "wait") == 0){
    return cmd_wait(argc, argv);
  }
  if(strcmp(argv[0], "kill") == 0){
    return cmd_kill(argc, argv);
  }
  if(strcmp(argv[0], "fg") == 0){
    return cmd_fg(argc, argv);
  }
  if(strcmp(argv[0], "bg") == 0){
    return cmd_bg(argc, argv);
  }
  if(strcmp(argv[0], "kmod") == 0){
    return cmd_kmod(argc, argv);
  }
  if(strcmp(argv[0], "time") == 0){
    return cmd_time(argc, argv, run_bg);
  }
  if(strcmp(argv[0], "ulimit") == 0){
    return cmd_ulimit(argc, argv);
  }
  if(strcmp(argv[0], "limit") == 0){
    return cmd_limit(argc, argv, run_bg);
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

  if(run_bg && is_builtin_command(argv[0]) && strcmp(argv[0], "time") != 0 && strcmp(argv[0], "limit") != 0){
    puts_line("bg: builtin not supported");
    return 2;
  }

  if(is_builtin_command(argv[0])){
    base_io.in_fd = 0;
    base_io.out_fd = 1;
    base_io.err_fd = 2;
    int parse_status = 1;
    ksh_stdio_state_t stdio_saved;
    int stdio_swapped = 0;
    if(parse_exec_and_redir(argc, argv, &base_io, cmd_argv, KSH_MAX_ARGS, &cmd_argc, &io, &parse_status) != 0)
      return parse_status;
    ksh_stdio_state_capture(&stdio_saved);
    if(xv6_stdio_set_fds(io.in_fd, io.out_fd, io.err_fd) != 0){
      close_io_custom_fds(&io, &base_io);
      puts_line("exec: no task context");
      return 1;
    }
    stdio_swapped = 1;
    {
      int rc = dispatch_builtin_command(cmd_argc, cmd_argv, run_bg);
      if(stdio_swapped)
        ksh_stdio_state_restore(&stdio_saved);
      close_io_custom_fds(&io, &base_io);
      if(rc < 0)
        return 1;
      return rc & 0xff;
    }
  }

  return execute_external(argc, argv, run_bg, g_ulimit_heap_kb, g_ulimit_ms);
}

static int eval_line_inner(const char *line, int *exit_code)
{
  char buf[256];
  char *argv[KSH_MAX_ARGS];
  int argc;
  int run_bg = 0;
  size_t n;
  int rc;

  if(line == 0)
    return -1;

  n = strlen(line);
  if(n >= sizeof(buf) - 1u){
    puts_line("parse: line too long");
    if(exit_code)
      *exit_code = 2;
    return -1;
  }
  memcpy(buf, line, n);
  buf[n] = 0;

  while(n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n')){
    buf[n - 1] = 0;
    n--;
  }

  argc = parse_line(buf, argv, KSH_MAX_ARGS);
  if(argc < 0){
    if(argc == -2){
      puts_line("parse: too many args");
      if(exit_code)
        *exit_code = 2;
      return -1;
    }
    puts_line("parse: unterminated quote");
    if(exit_code)
      *exit_code = 2;
    return -1;
  }
  if(argc == 0){
    if(exit_code)
      *exit_code = 0;
    return 0;
  }

  if(strcmp(argv[argc - 1], "&") == 0){
    run_bg = 1;
    argc--;
    if(argc == 0){
      puts_line("syntax: command &");
      if(exit_code)
        *exit_code = 2;
      return -1;
    }
  }

  rc = dispatch_command(argc, argv, run_bg);
  if(exit_code)
    *exit_code = (rc >= 0) ? rc : 1;
  return (rc < 0) ? -1 : 0;
}

int shell_runtime_init(void)
{
  kmod_config_t kcfg;

  if(__sync_lock_test_and_set(&g_runtime_started, 1) != 0)
    return 0;

  if(ksh_jobs_ensure() != 0)
    goto fail;

  if(xv6_network_runtime_init() != 0){
    eputs_line("network runtime init failed");
    goto fail;
  }

  elf_loader_init();
  g_jobs_lock = xSemaphoreCreateMutex();
  if(g_jobs_lock == 0)
    goto fail;
  g_interactive_task = 0;
  if(register_default_symbols() != 0){
    eputs_line("hostabi: failed to register default exports");
    goto fail;
  }
  xv6_vfs_reset();
  hostabi_posix_io_init();
  hostabi_posix_set_tty_signal_handler(k_tty_signal_handler);
  memset(&kcfg, 0, sizeof(kcfg));
  kcfg.dev_mode = XV6_KMOD_DEV_MODE_DEFAULT;
  kcfg.allow_unsigned_dev = XV6_KMOD_ALLOW_UNSIGNED_DEV_DEFAULT;
  kcfg.allow_force_unload_dev = XV6_KMOD_ALLOW_FORCE_UNLOAD_DEV_DEFAULT;
  if(kmod_init(&kcfg) != 0)
    goto fail;
  if(kmod_autoload_from_manifest(0, 0) != 0){
    eputs_console("kmod autoload: ");
    eputs_line(kmod_last_error());
  }
  (void)xv6_chdir("/");
  env_init_defaults();
  return 0;

fail:
  if(g_jobs_lock){
    vSemaphoreDelete(g_jobs_lock);
    g_jobs_lock = 0;
  }
  ksh_jobs_release();
  g_runtime_started = 0;
  return -1;
}

int shell_runtime_eval_line(const char *line, int *exit_code)
{
  if(shell_runtime_init() != 0)
    return -1;
  if(g_interactive_task == 0){
    g_interactive_task = xTaskGetCurrentTaskHandle();
    if(g_jobs_lock)
      (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
    g_tty_fg_pgid = g_shell_pgid;
    if(g_jobs_lock)
      (void)xSemaphoreGive(g_jobs_lock);
  }
  return eval_line_inner(line, exit_code);
}

int shell_runtime_bootstrap(const char *shell_path)
{
  char *argv[2];
  int exit_code = 127;

  if(shell_runtime_init() != 0)
    return -1;

  if(shell_path == 0 || shell_path[0] == 0)
    shell_path = "/bin/sh";

  argv[0] = (char *)shell_path;
  argv[1] = 0;
  return run_elf_command(1, argv, &exit_code, 0, 1, 2, 0);
}

void shell_runtime_reboot(void)
{
  hal_reboot();
}

int shell_runtime_run_interactive(void)
{
  char line[256];
  int len = 0;

  if(shell_runtime_init() != 0)
    return -1;

  g_interactive_task = xTaskGetCurrentTaskHandle();
  if(g_jobs_lock)
    (void)xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  g_tty_fg_pgid = g_shell_pgid;
  if(g_jobs_lock)
    (void)xSemaphoreGive(g_jobs_lock);
  tty_puts("xv6> ");
  for(;;){
    int c;
    int ldisc_sig = 0;

    enforce_job_limits();
    (void)k_signal_dispatch_pending_current();

    c = hal_console_getc();
    if(c < 0){
      hal_delay_ms(5);
      continue;
    }

    ldisc_sig = hostabi_posix_tty_signal_for_char(c);
    if(ldisc_sig == 0 && c != '\r' && c != '\n' && c != '\b' && c != 0x7f && c != '\t' && (c < 0x20 || c > 0x7e))
    {
      taskYIELD();
      continue;
    }

    if(ldisc_sig == SIGINT){
      puts_line("^C");
      len = 0;
      tty_puts("xv6> ");
      continue;
    }

    if(ldisc_sig == SIGTSTP){
      puts_line("^Z");
      len = 0;
      tty_puts("xv6> ");
      continue;
    }

    if(c == '\r' || c == '\n'){
      int exit_code = 0;
      line[len] = 0;
      puts_line("");
      (void)eval_line_inner(line, &exit_code);
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
    taskYIELD();
  }
}
