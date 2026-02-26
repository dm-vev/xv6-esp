#include "ducktape_engine_api.h"

#include <duktape.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DTK_ERR_LEN 96

struct dtk_engine {
  duk_context *ctx;
  char err[DTK_ERR_LEN];
};

static void set_error(dtk_engine_t *eng, const char *msg)
{
  if (eng == NULL) {
    return;
  }
  snprintf(eng->err, sizeof(eng->err), "%s", msg ? msg : "unknown error");
}

static void clear_error(dtk_engine_t *eng)
{
  if (eng == NULL) {
    return;
  }
  eng->err[0] = '\0';
}

static int dtk_bind_number_impl(dtk_engine_t *engine, const char *name, double value)
{
  if (engine == NULL || engine->ctx == NULL || name == NULL || name[0] == '\0') {
    return -1;
  }

  duk_push_number(engine->ctx, value);
  duk_put_global_string(engine->ctx, name);
  return 0;
}

static const dtk_engine_api_t g_api = {
  .bind_number = dtk_bind_number_impl,
};

dtk_engine_t *dtk_engine_create(void)
{
  dtk_engine_t *eng;

  eng = (dtk_engine_t *)calloc(1, sizeof(*eng));
  if (eng == NULL) {
    return NULL;
  }

  eng->ctx = duk_create_heap_default();
  if (eng->ctx == NULL) {
    free(eng);
    return NULL;
  }

  clear_error(eng);
  return eng;
}

void dtk_engine_destroy(dtk_engine_t *engine)
{
  if (engine == NULL) {
    return;
  }
  if (engine->ctx != NULL) {
    duk_destroy_heap(engine->ctx);
  }
  free(engine);
}

int dtk_engine_eval_number(dtk_engine_t *engine, const char *expr, double *out)
{
  if (engine == NULL || engine->ctx == NULL || expr == NULL || out == NULL) {
    return -1;
  }

  clear_error(engine);
  if (duk_peval_string(engine->ctx, expr) != 0) {
    const char *err = duk_safe_to_string(engine->ctx, -1);
    set_error(engine, err);
    duk_pop(engine->ctx);
    return -1;
  }

  if (!duk_is_number(engine->ctx, -1)) {
    set_error(engine, "expression result is not a number");
    duk_pop(engine->ctx);
    return -1;
  }

  *out = duk_get_number(engine->ctx, -1);
  duk_pop(engine->ctx);
  return 0;
}

const char *dtk_engine_last_error(dtk_engine_t *engine)
{
  if (engine == NULL || engine->err[0] == '\0') {
    return "ok";
  }
  return engine->err;
}

const dtk_engine_api_t *dtk_engine_api(void)
{
  return &g_api;
}

const char *dtk_engine_name(void)
{
  return "duktape";
}

int dtk_engine_is_stub(void)
{
  return 0;
}
