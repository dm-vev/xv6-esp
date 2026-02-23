#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define XARGS_LINE_LIMIT 240
#define XARGS_MAX_ARGS   31

extern int shrt_eval_line(const char *line, int *exit_code);

static void xargs_usage(void)
{
  fprintf(stderr, "usage: xargs [-0] [-r] [-n max-args] [command [arg ...]]\n");
}

static int needs_quote(const char *s)
{
  const char *p = s;
  if(*s == 0)
    return 1;
  while(*p){
    unsigned char c = (unsigned char)*p;
    if(isspace(c))
      return 1;
    if(strchr("|&;<>()[{}]*?!$`\\\"'", (int)c))
      return 1;
    p++;
  }
  return 0;
}

static size_t quoted_len(const char *s)
{
  size_t n = 0;
  const char *p = s;

  if(!needs_quote(s))
    return strlen(s);

  n += 2;
  while(*p){
    if(*p == '\'')
      n += 4;
    else
      n += 1;
    p++;
  }
  return n;
}

static int append_char(char *dst, size_t dstsz, size_t *used, char c)
{
  if(*used + 1 >= dstsz)
    return -1;
  dst[*used] = c;
  (*used)++;
  dst[*used] = 0;
  return 0;
}

static int append_text(char *dst, size_t dstsz, size_t *used, const char *text)
{
  size_t left = dstsz - *used;
  int n;

  if(left == 0)
    return -1;
  n = snprintf(dst + *used, left, "%s", text);
  if(n < 0 || (size_t)n >= left)
    return -1;
  *used += (size_t)n;
  return 0;
}

static int append_quoted(char *dst, size_t dstsz, size_t *used, const char *s)
{
  const char *p = s;
  if(!needs_quote(s))
    return append_text(dst, dstsz, used, s);

  if(append_char(dst, dstsz, used, '\'') != 0)
    return -1;
  while(*p){
    if(*p == '\''){
      if(append_text(dst, dstsz, used, "'\\''") != 0)
        return -1;
    } else {
      if(append_char(dst, dstsz, used, *p) != 0)
        return -1;
    }
    p++;
  }
  return append_char(dst, dstsz, used, '\'');
}

static size_t args_len(char **argv, int argc)
{
  size_t len = 0;
  int i;

  for(i = 0; i < argc; i++){
    if(i > 0)
      len += 1;
    len += quoted_len(argv[i]);
  }
  return len;
}

static int run_batch(char **cmdv, int cmdc, char **extras, int nextras, int *out_exit_code)
{
  char line[XARGS_LINE_LIMIT + 1];
  size_t used = 0;
  int i;
  int exit_code = 0;
  int rc;

  line[0] = 0;
  for(i = 0; i < cmdc; i++){
    if(i > 0 && append_char(line, sizeof(line), &used, ' ') != 0)
      goto toolong;
    if(append_quoted(line, sizeof(line), &used, cmdv[i]) != 0)
      goto toolong;
  }
  for(i = 0; i < nextras; i++){
    if(append_char(line, sizeof(line), &used, ' ') != 0)
      goto toolong;
    if(append_quoted(line, sizeof(line), &used, extras[i]) != 0)
      goto toolong;
  }

  rc = shrt_eval_line(line, &exit_code);
  if(rc != 0){
    fprintf(stderr, "xargs: command execution failed\n");
    return 1;
  }
  if(out_exit_code)
    *out_exit_code = exit_code;
  return 0;

toolong:
  fprintf(stderr, "xargs: command line too long\n");
  return 1;
}

static int read_token_ws(char **out)
{
  int c;
  int in_single = 0;
  int in_double = 0;
  int started = 0;
  size_t cap = 64;
  size_t len = 0;
  char *buf = (char *)malloc(cap);

  if(buf == 0)
    return -1;

  for(;;){
    c = getchar();
    if(c == EOF){
      if(in_single || in_double){
        free(buf);
        fprintf(stderr, "xargs: unterminated quote\n");
        return -2;
      }
      if(!started){
        free(buf);
        return 0;
      }
      break;
    }

    if(!in_single && !in_double && isspace((unsigned char)c)){
      if(!started)
        continue;
      break;
    }

    if(!in_double && c == '\''){
      in_single = !in_single;
      started = 1;
      continue;
    }
    if(!in_single && c == '"'){
      in_double = !in_double;
      started = 1;
      continue;
    }
    if(!in_single && !in_double && c == '\\'){
      c = getchar();
      if(c == EOF){
        free(buf);
        fprintf(stderr, "xargs: backslash at end of input\n");
        return -2;
      }
    }

    if(len + 1 >= cap){
      size_t ncap = cap * 2;
      char *nbuf = (char *)realloc(buf, ncap);
      if(nbuf == 0){
        free(buf);
        return -1;
      }
      buf = nbuf;
      cap = ncap;
    }
    buf[len++] = (char)c;
    started = 1;
  }

  buf[len] = 0;
  *out = buf;
  return 1;
}

static int read_token_nul(char **out)
{
  int c;
  size_t cap = 64;
  size_t len = 0;
  char *buf = (char *)malloc(cap);

  if(buf == 0)
    return -1;

  c = getchar();
  if(c == EOF){
    free(buf);
    return 0;
  }

  while(c != EOF && c != '\0'){
    if(len + 1 >= cap){
      size_t ncap = cap * 2;
      char *nbuf = (char *)realloc(buf, ncap);
      if(nbuf == 0){
        free(buf);
        return -1;
      }
      buf = nbuf;
      cap = ncap;
    }
    buf[len++] = (char)c;
    c = getchar();
  }

  buf[len] = 0;
  *out = buf;
  return 1;
}

static int parse_positive_int(const char *s, int *out)
{
  char *end = 0;
  long v;

  if(s == 0 || *s == 0)
    return -1;
  errno = 0;
  v = strtol(s, &end, 10);
  if(errno != 0 || end == s || *end != 0 || v <= 0)
    return -1;
  *out = (int)v;
  return 0;
}

static void free_tokens(char **tokens, int ntokens)
{
  int i;
  if(tokens == 0)
    return;
  for(i = 0; i < ntokens; i++)
    free(tokens[i]);
  free(tokens);
}

int main(int argc, char **argv)
{
  int use_nul = 0;
  int no_run_if_empty = 0;
  int max_args = 0;
  int opt;
  int cmdc;
  char **cmdv;
  char *default_cmd[] = { "echo", 0 };
  int hard_batch_limit;
  int batch_limit;
  size_t base_len;
  char **tokens = 0;
  int ntokens = 0;
  int cap_tokens = 0;
  int saw_token = 0;
  int idx;

  while((opt = getopt(argc, argv, "0rn:h")) != -1){
    switch(opt){
    case '0':
      use_nul = 1;
      break;
    case 'r':
      no_run_if_empty = 1;
      break;
    case 'n':
      if(parse_positive_int(optarg, &max_args) != 0){
        fprintf(stderr, "xargs: invalid -n value '%s'\n", optarg ? optarg : "");
        return 1;
      }
      break;
    case 'h':
      xargs_usage();
      return 0;
    default:
      xargs_usage();
      return 1;
    }
  }

  cmdc = argc - optind;
  cmdv = argv + optind;
  if(cmdc <= 0){
    cmdv = default_cmd;
    cmdc = 1;
  }

  hard_batch_limit = XARGS_MAX_ARGS - cmdc;
  if(hard_batch_limit < 0){
    fprintf(stderr, "xargs: too many command arguments\n");
    return 1;
  }

  batch_limit = max_args > 0 ? max_args : hard_batch_limit;
  if(batch_limit > hard_batch_limit)
    batch_limit = hard_batch_limit;
  if(batch_limit <= 0){
    fprintf(stderr, "xargs: no room for input arguments\n");
    return 1;
  }

  base_len = args_len(cmdv, cmdc);
  if(base_len > XARGS_LINE_LIMIT){
    fprintf(stderr, "xargs: base command is too long\n");
    return 1;
  }

  for(;;){
    char *tok = 0;
    int r = use_nul ? read_token_nul(&tok) : read_token_ws(&tok);

    if(r < 0){
      if(r == -2){
        free_tokens(tokens, ntokens);
        return 1;
      }
      fprintf(stderr, "xargs: out of memory\n");
      free_tokens(tokens, ntokens);
      return 1;
    }
    if(r == 0)
      break;

    saw_token = 1;

    {
      size_t tok_part = 1 + quoted_len(tok);

      if(base_len + tok_part > XARGS_LINE_LIMIT){
        fprintf(stderr, "xargs: single argument too long\n");
        free(tok);
        free_tokens(tokens, ntokens);
        return 1;
      }

      if(ntokens >= cap_tokens){
        int ncap = cap_tokens ? cap_tokens * 2 : 16;
        char **nt = (char **)realloc(tokens, (size_t)ncap * sizeof(*tokens));
        if(nt == 0){
          fprintf(stderr, "xargs: out of memory\n");
          free(tok);
          free_tokens(tokens, ntokens);
          return 1;
        }
        tokens = nt;
        cap_tokens = ncap;
      }
      tokens[ntokens++] = tok;
    }
  }

  if(saw_token)
    usleep(10000);

  if(ntokens == 0){
    int rc = 0;
    if(!saw_token && !no_run_if_empty){
      int exit_code = 0;
      rc = run_batch(cmdv, cmdc, 0, 0, &exit_code);
      if(rc == 0)
        rc = exit_code & 0xff;
    }
    free(tokens);
    return rc;
  }

  idx = 0;
  while(idx < ntokens){
    int batch = 0;
    size_t extra_len = 0;

    while(idx + batch < ntokens && batch < batch_limit){
      size_t tok_part = 1 + quoted_len(tokens[idx + batch]);
      if(batch > 0 && base_len + extra_len + tok_part > XARGS_LINE_LIMIT)
        break;
      if(batch == 0 && base_len + tok_part > XARGS_LINE_LIMIT){
        fprintf(stderr, "xargs: single argument too long\n");
        free_tokens(tokens, ntokens);
        return 1;
      }
      extra_len += tok_part;
      batch++;
    }

    if(batch <= 0){
      fprintf(stderr, "xargs: internal batching error\n");
      free_tokens(tokens, ntokens);
      return 1;
    }

    {
      int exit_code = 0;
      int run_rc = run_batch(cmdv, cmdc, tokens + idx, batch, &exit_code);
      if(run_rc != 0){
        free_tokens(tokens, ntokens);
        return run_rc;
      }
      if(exit_code != 0){
        free_tokens(tokens, ntokens);
        return exit_code & 0xff;
      }
    }

    idx += batch;
  }

  free_tokens(tokens, ntokens);
  return 0;
}
