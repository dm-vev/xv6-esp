#ifndef DUKTAPE_ENGINE_API_H
#define DUKTAPE_ENGINE_API_H

#include <stddef.h>

typedef struct dtk_engine dtk_engine_t;

typedef struct {
  int (*bind_number)(dtk_engine_t *engine, const char *name, double value);
} dtk_engine_api_t;

typedef dtk_engine_t *(*dtk_engine_create_fn_t)(void);
typedef void (*dtk_engine_destroy_fn_t)(dtk_engine_t *engine);
typedef int (*dtk_engine_eval_number_fn_t)(dtk_engine_t *engine, const char *expr, double *out);
typedef const char *(*dtk_engine_last_error_fn_t)(dtk_engine_t *engine);
typedef const dtk_engine_api_t *(*dtk_engine_api_fn_t)(void);
typedef const char *(*dtk_engine_name_fn_t)(void);
typedef int (*dtk_engine_is_stub_fn_t)(void);

typedef int (*dtk_module_init_fn_t)(dtk_engine_t *engine, const dtk_engine_api_t *api);

#endif
