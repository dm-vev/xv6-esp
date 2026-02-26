#include "ducktape_engine_api.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DTK_ENGINE_SO "/lib/duktape/dt_eng.so"
#define DTK_MOD_MATH_SO "/lib/duktape/dt_math.so"
#define DTK_MOD_META_SO "/lib/duktape/dt_meta.so"
#define DTK_REPL_LINE_MAX 256
#define DTK_PROMPT "duktape> "

typedef enum {
  DTK_REPL_LINE_OK = 0,
  DTK_REPL_LINE_EOF = 1,
  DTK_REPL_LINE_INTERRUPT = 2,
  DTK_REPL_LINE_OVERFLOW = 3,
} dtk_repl_line_status_t;

static int load_module(const char *path, dtk_engine_t *engine, const dtk_engine_api_t *api)
{
  void *mod;
  dtk_module_init_fn_t init_fn;

  mod = dlopen(path, RTLD_NOW);
  if (mod == NULL) {
    const char *err = dlerror();
    fprintf(stderr, "duktape_demo: dlopen(%s) failed: %s\n", path, err ? err : "unknown");
    return -1;
  }

  init_fn = (dtk_module_init_fn_t)dlsym(mod, "dtk_module_init");
  if (init_fn == NULL) {
    const char *err = dlerror();
    fprintf(stderr, "duktape_demo: dlsym dtk_module_init(%s) failed: %s\n", path, err ? err : "unknown");
    (void)dlclose(mod);
    return -1;
  }

  if (init_fn(engine, api) != 0) {
    fprintf(stderr, "duktape_demo: module init failed for %s\n", path);
    (void)dlclose(mod);
    return -1;
  }

  if (dlclose(mod) != 0) {
    const char *err = dlerror();
    fprintf(stderr, "duktape_demo: dlclose(%s) failed: %s\n", path, err ? err : "unknown");
    return -1;
  }

  return 0;
}

static int eval_and_print(dtk_engine_t *engine,
                          dtk_engine_eval_number_fn_t engine_eval,
                          dtk_engine_last_error_fn_t engine_last_error,
                          dtk_engine_name_fn_t engine_name,
                          dtk_engine_is_stub_fn_t engine_is_stub,
                          const char *expr,
                          int repl_mode)
{
  double value;

  if (engine_eval(engine, expr, &value) != 0) {
    fprintf(stderr,
            "duktape_demo: eval failed (%s): %s\n",
            engine_name(),
            engine_last_error(engine));
    if (engine_is_stub()) {
      fprintf(stderr,
              "duktape_demo: stub mode expects arithmetic expressions and known constants (PI,E,MAGIC,ONE)\n");
    }
    return -1;
  }

  if (repl_mode) {
    printf("%g\n", value);
  } else {
    printf("duktape_demo: engine=%s mode=%s expr=\"%s\" result=%g\n",
           engine_name(),
           engine_is_stub() ? "stub" : "real",
           expr,
           value);
  }
  return 0;
}

static void drain_stdin_until_eol(void)
{
  unsigned char ch = 0;
  ssize_t nread;

  for (;;) {
    nread = read(STDIN_FILENO, &ch, 1);
    if (nread != 1) {
      return;
    }
    if (ch == '\n' || ch == '\r' || ch == 0x03 || ch == 0x04) {
      return;
    }
  }
}

static dtk_repl_line_status_t read_repl_line(char *line, size_t cap)
{
  size_t len = 0;

  if (line == NULL || cap < 2) {
    return DTK_REPL_LINE_EOF;
  }

  for (;;) {
    unsigned char ch = 0;
    ssize_t nread = read(STDIN_FILENO, &ch, 1);
    if (nread == 0) {
      line[len] = '\0';
      return DTK_REPL_LINE_EOF;
    }
    if (nread < 0) {
      line[len] = '\0';
      return DTK_REPL_LINE_EOF;
    }

    if (ch == 0x03) {
      (void)write(STDOUT_FILENO, "^C\r\n", 4);
      line[0] = '\0';
      return DTK_REPL_LINE_INTERRUPT;
    }
    if (ch == 0x04) {
      line[len] = '\0';
      return DTK_REPL_LINE_EOF;
    }
    if (ch == '\n' || ch == '\r') {
      (void)write(STDOUT_FILENO, "\r\n", 2);
      line[len] = '\0';
      return DTK_REPL_LINE_OK;
    }
    if (ch == 0x08 || ch == 0x7f) {
      if (len > 0) {
        len--;
        (void)write(STDOUT_FILENO, "\b \b", 3);
      }
      continue;
    }
    if (ch < 0x20 || ch > 0x7e) {
      continue;
    }
    if (len + 1 >= cap) {
      (void)write(STDOUT_FILENO, "\r\n", 2);
      line[0] = '\0';
      drain_stdin_until_eol();
      return DTK_REPL_LINE_OVERFLOW;
    }

    line[len++] = (char)ch;
    (void)write(STDOUT_FILENO, (const char *)&ch, 1);
  }
}

static char *join_args(int argc, char **argv, int first)
{
  char *buf;
  size_t total = 0;
  int i;
  int pos = 0;

  for (i = first; i < argc; i++) {
    total += strlen(argv[i]) + 1;
  }

  buf = (char *)malloc(total + 1);
  if (buf == NULL) {
    return NULL;
  }

  buf[0] = '\0';
  for (i = first; i < argc; i++) {
    int written = snprintf(buf + pos, total + 1 - (size_t)pos, "%s%s", (i == first) ? "" : " ", argv[i]);
    if (written < 0) {
      free(buf);
      return NULL;
    }
    pos += written;
  }

  return buf;
}

static int run_repl(dtk_engine_t *engine,
                    dtk_engine_eval_number_fn_t engine_eval,
                    dtk_engine_last_error_fn_t engine_last_error,
                    dtk_engine_name_fn_t engine_name,
                    dtk_engine_is_stub_fn_t engine_is_stub)
{
  char line[DTK_REPL_LINE_MAX];
  dtk_repl_line_status_t status;

  printf("duktape_demo REPL (%s, mode=%s)\n", engine_name(), engine_is_stub() ? "stub" : "real");
  printf("type expression, or 'exit'/'quit' to leave\n");

  for (;;) {
    if (write(STDOUT_FILENO, DTK_PROMPT, sizeof(DTK_PROMPT) - 1) < 0) {
      return 0;
    }

    status = read_repl_line(line, sizeof(line));
    if (status == DTK_REPL_LINE_INTERRUPT || status == DTK_REPL_LINE_EOF) {
      (void)write(STDOUT_FILENO, "\r\n", 2);
      return 0;
    }
    if (status == DTK_REPL_LINE_OVERFLOW) {
      fprintf(stderr, "duktape_demo: input line too long (max %d)\n", DTK_REPL_LINE_MAX - 1);
      continue;
    }

    if (line[0] == '\0') {
      continue;
    }
    if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
      return 0;
    }

    (void)eval_and_print(engine,
                         engine_eval,
                         engine_last_error,
                         engine_name,
                         engine_is_stub,
                         line,
                         1);
  }
}

int main(int argc, char **argv)
{
  void *engine_lib;
  dtk_engine_create_fn_t engine_create;
  dtk_engine_destroy_fn_t engine_destroy;
  dtk_engine_eval_number_fn_t engine_eval;
  dtk_engine_last_error_fn_t engine_last_error;
  dtk_engine_api_fn_t engine_api_get;
  dtk_engine_name_fn_t engine_name;
  dtk_engine_is_stub_fn_t engine_is_stub;
  dtk_engine_t *engine;
  const dtk_engine_api_t *api;
  char *expr = NULL;
  int rc = 0;

  /* xv6 serial stdio may keep prompts buffered without a trailing newline. */
  (void)setvbuf(stdout, NULL, _IONBF, 0);
  (void)setvbuf(stderr, NULL, _IONBF, 0);

  if (argc > 1) {
    expr = join_args(argc, argv, 1);
    if (expr == NULL) {
      fprintf(stderr, "duktape_demo: failed to allocate expression buffer\n");
      return 1;
    }
  }

  engine_lib = dlopen(DTK_ENGINE_SO, RTLD_NOW);
  if (engine_lib == NULL) {
    const char *err = dlerror();
    fprintf(stderr, "duktape_demo: dlopen(%s) failed: %s\n", DTK_ENGINE_SO, err ? err : "unknown");
    free(expr);
    return 1;
  }

  engine_create = (dtk_engine_create_fn_t)dlsym(engine_lib, "dtk_engine_create");
  engine_destroy = (dtk_engine_destroy_fn_t)dlsym(engine_lib, "dtk_engine_destroy");
  engine_eval = (dtk_engine_eval_number_fn_t)dlsym(engine_lib, "dtk_engine_eval_number");
  engine_last_error = (dtk_engine_last_error_fn_t)dlsym(engine_lib, "dtk_engine_last_error");
  engine_api_get = (dtk_engine_api_fn_t)dlsym(engine_lib, "dtk_engine_api");
  engine_name = (dtk_engine_name_fn_t)dlsym(engine_lib, "dtk_engine_name");
  engine_is_stub = (dtk_engine_is_stub_fn_t)dlsym(engine_lib, "dtk_engine_is_stub");

  if (engine_create == NULL || engine_destroy == NULL || engine_eval == NULL ||
      engine_last_error == NULL || engine_api_get == NULL || engine_name == NULL ||
      engine_is_stub == NULL) {
    const char *err = dlerror();
    fprintf(stderr, "duktape_demo: engine symbols are incomplete: %s\n", err ? err : "unknown");
    rc = 2;
    goto out_close_engine_lib;
  }

  engine = engine_create();
  if (engine == NULL) {
    fprintf(stderr, "duktape_demo: failed to create engine\n");
    rc = 3;
    goto out_close_engine_lib;
  }

  api = engine_api_get();
  if (api == NULL) {
    fprintf(stderr, "duktape_demo: failed to fetch engine API\n");
    rc = 4;
    goto out_destroy_engine;
  }

  if (load_module(DTK_MOD_MATH_SO, engine, api) != 0 ||
      load_module(DTK_MOD_META_SO, engine, api) != 0) {
    rc = 5;
    goto out_destroy_engine;
  }

  if (expr != NULL) {
    rc = (eval_and_print(engine,
                         engine_eval,
                         engine_last_error,
                         engine_name,
                         engine_is_stub,
                         expr,
                         0) == 0)
           ? 0
           : 6;
  } else {
    rc = run_repl(engine, engine_eval, engine_last_error, engine_name, engine_is_stub);
  }

out_destroy_engine:
  engine_destroy(engine);
out_close_engine_lib:
  if (dlclose(engine_lib) != 0) {
    const char *err = dlerror();
    fprintf(stderr, "duktape_demo: dlclose engine failed: %s\n", err ? err : "unknown");
    if (rc == 0) {
      rc = 7;
    }
  }
  free(expr);
  return rc;
}
