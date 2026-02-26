#include "ducktape_engine_api.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DTK_MAX_SYMBOLS 32
#define DTK_ERR_LEN 96

typedef struct {
  char name[24];
  double value;
} dtk_sym_t;

struct dtk_engine {
  dtk_sym_t symbols[DTK_MAX_SYMBOLS];
  int symbol_count;
  char err[DTK_ERR_LEN];
};

typedef struct {
  struct dtk_engine *eng;
  const char *p;
  int ok;
} parser_t;

static void set_error(struct dtk_engine *eng, const char *msg)
{
  if (eng == NULL) {
    return;
  }
  snprintf(eng->err, sizeof(eng->err), "%s", msg ? msg : "unknown error");
}

static void clear_error(struct dtk_engine *eng)
{
  if (eng == NULL) {
    return;
  }
  eng->err[0] = '\0';
}

static void skip_ws(parser_t *ps)
{
  while (*ps->p != '\0' && isspace((unsigned char)*ps->p)) {
    ps->p++;
  }
}

static double parse_expr(parser_t *ps);

static int lookup_symbol(struct dtk_engine *eng, const char *name, size_t nlen, double *out)
{
  int i;

  if (eng == NULL || name == NULL || out == NULL || nlen == 0) {
    return -1;
  }

  for (i = 0; i < eng->symbol_count; i++) {
    if (strncmp(eng->symbols[i].name, name, nlen) == 0 && eng->symbols[i].name[nlen] == '\0') {
      *out = eng->symbols[i].value;
      return 0;
    }
  }
  return -1;
}

static double parse_primary(parser_t *ps)
{
  char *endp;
  double val;

  skip_ws(ps);
  if (!ps->ok) {
    return 0.0;
  }

  if (*ps->p == '(') {
    ps->p++;
    val = parse_expr(ps);
    skip_ws(ps);
    if (*ps->p != ')') {
      set_error(ps->eng, "expected ')' in expression");
      ps->ok = 0;
      return 0.0;
    }
    ps->p++;
    return val;
  }

  if (isalpha((unsigned char)*ps->p) || *ps->p == '_') {
    const char *start = ps->p;
    size_t nlen;

    ps->p++;
    while (isalnum((unsigned char)*ps->p) || *ps->p == '_') {
      ps->p++;
    }
    nlen = (size_t)(ps->p - start);
    if (lookup_symbol(ps->eng, start, nlen, &val) != 0) {
      set_error(ps->eng, "unknown identifier in expression");
      ps->ok = 0;
      return 0.0;
    }
    return val;
  }

  val = strtod(ps->p, &endp);
  if (endp == ps->p) {
    set_error(ps->eng, "expected number in expression");
    ps->ok = 0;
    return 0.0;
  }
  ps->p = endp;
  return val;
}

static double parse_term(parser_t *ps)
{
  double lhs = parse_primary(ps);

  while (ps->ok) {
    double rhs;
    char op;

    skip_ws(ps);
    op = *ps->p;
    if (op != '*' && op != '/') {
      break;
    }
    ps->p++;
    rhs = parse_primary(ps);
    if (!ps->ok) {
      break;
    }
    if (op == '*') {
      lhs *= rhs;
    } else {
      if (rhs == 0.0) {
        set_error(ps->eng, "division by zero");
        ps->ok = 0;
        break;
      }
      lhs /= rhs;
    }
  }

  return lhs;
}

static double parse_expr(parser_t *ps)
{
  double lhs = parse_term(ps);

  while (ps->ok) {
    double rhs;
    char op;

    skip_ws(ps);
    op = *ps->p;
    if (op != '+' && op != '-') {
      break;
    }
    ps->p++;
    rhs = parse_term(ps);
    if (!ps->ok) {
      break;
    }
    if (op == '+') {
      lhs += rhs;
    } else {
      lhs -= rhs;
    }
  }

  return lhs;
}

static int dtk_bind_number_impl(dtk_engine_t *engine, const char *name, double value)
{
  int i;
  size_t nlen;

  if (engine == NULL || name == NULL || name[0] == '\0') {
    return -1;
  }

  nlen = strlen(name);
  if (nlen >= sizeof(engine->symbols[0].name)) {
    set_error(engine, "symbol name is too long");
    return -1;
  }

  for (i = 0; i < engine->symbol_count; i++) {
    if (strcmp(engine->symbols[i].name, name) == 0) {
      engine->symbols[i].value = value;
      return 0;
    }
  }

  if (engine->symbol_count >= DTK_MAX_SYMBOLS) {
    set_error(engine, "symbol table is full");
    return -1;
  }

  snprintf(engine->symbols[engine->symbol_count].name,
           sizeof(engine->symbols[engine->symbol_count].name),
           "%s",
           name);
  engine->symbols[engine->symbol_count].value = value;
  engine->symbol_count++;
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
  clear_error(eng);
  return eng;
}

void dtk_engine_destroy(dtk_engine_t *engine)
{
  free(engine);
}

int dtk_engine_eval_number(dtk_engine_t *engine, const char *expr, double *out)
{
  parser_t ps;
  double val;

  if (engine == NULL || expr == NULL || out == NULL) {
    return -1;
  }

  clear_error(engine);
  ps.eng = engine;
  ps.p = expr;
  ps.ok = 1;

  val = parse_expr(&ps);
  skip_ws(&ps);
  if (!ps.ok) {
    return -1;
  }
  if (*ps.p != '\0') {
    set_error(engine, "unexpected trailing characters");
    return -1;
  }

  *out = val;
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
  return "stub";
}

int dtk_engine_is_stub(void)
{
  return 1;
}
