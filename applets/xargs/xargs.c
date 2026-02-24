#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define XARGS_LINE_LIMIT 240
#define XARGS_MAX_ARGS   31
#define XARGS_TOKEN_MAX  XARGS_LINE_LIMIT

extern int shrt_eval_line(const char *line, int *exit_code);

static void xargs_usage(void)
{
  fprintf(stderr, "usage: xargs [-0] [-r] [-n max-args] [command [arg ...]]\n");
}

static int parse_positive_int(const char *s, int *out)
{
  int v = 0;
  int i = 0;

  if(s == 0 || *s == 0)
    return -1;
  while(s[i] != 0){
    int d;
    if(s[i] < '0' || s[i] > '9')
      return -1;
    d = s[i] - '0';
    if(v > 100000)
      return -1;
    v = v * 10 + d;
    i++;
  }
  if(v <= 0)
    return -1;
  *out = v;
  return 0;
}

static int append_part(char *line, size_t line_cap, size_t *used, const char *part)
{
  size_t len = strlen(part);

  if(*used != 0){
    if(*used + 1 >= line_cap)
      return -1;
    line[*used] = ' ';
    (*used)++;
  }
  if(*used + len >= line_cap)
    return -1;
  memcpy(line + *used, part, len);
  *used += len;
  line[*used] = 0;
  return 0;
}

static int run_line(const char *line)
{
  int exit_code = 0;

  if(shrt_eval_line(line, &exit_code) != 0){
    fprintf(stderr, "xargs: command execution failed\n");
    return 1;
  }
  if(exit_code != 0)
    return exit_code & 0xff;
  return 0;
}

static int read_next_token_ws(char *tok, size_t tok_cap)
{
  int c;
  size_t len = 0;

  do {
    c = getchar();
  } while(c != EOF && isspace((unsigned char)c));

  if(c == EOF)
    return 0;

  while(c != EOF && !isspace((unsigned char)c)){
    if(len + 1 >= tok_cap)
      return -1;
    tok[len++] = (char)c;
    c = getchar();
  }
  tok[len] = 0;
  return 1;
}

static int read_next_token_nul(char *tok, size_t tok_cap)
{
  int c;
  size_t len = 0;

  c = getchar();
  if(c == EOF)
    return 0;

  while(c != EOF && c != '\0'){
    if(len + 1 >= tok_cap)
      return -1;
    tok[len++] = (char)c;
    c = getchar();
  }

  while(len > 0 && (tok[len - 1] == '\n' || tok[len - 1] == '\r'))
    len--;
  tok[len] = 0;
  return 1;
}

static int parse_options(int argc, char **argv, int *out_argi, int *out_use_nul, int *out_no_run_if_empty, int *out_max_args)
{
  int i = 1;
  int use_nul = 0;
  int no_run_if_empty = 0;
  int max_args = 0;

  while(i < argc){
    if(strcmp(argv[i], "--") == 0){
      i++;
      break;
    }
    if(strcmp(argv[i], "-0") == 0){
      use_nul = 1;
      i++;
      continue;
    }
    if(strcmp(argv[i], "-r") == 0){
      no_run_if_empty = 1;
      i++;
      continue;
    }
    if(strcmp(argv[i], "-h") == 0){
      xargs_usage();
      return 1;
    }
    if(strcmp(argv[i], "-n") == 0){
      if(i + 1 >= argc){
        fprintf(stderr, "xargs: missing -n value\n");
        return -1;
      }
      if(parse_positive_int(argv[i + 1], &max_args) != 0){
        fprintf(stderr, "xargs: invalid -n value '%s'\n", argv[i + 1]);
        return -1;
      }
      i += 2;
      continue;
    }
    if(argv[i][0] == '-' && argv[i][1] != 0){
      fprintf(stderr, "xargs: unknown option '%s'\n", argv[i]);
      return -1;
    }
    break;
  }

  *out_argi = i;
  *out_use_nul = use_nul;
  *out_no_run_if_empty = no_run_if_empty;
  *out_max_args = max_args;
  return 0;
}

int main(int argc, char **argv)
{
  int use_nul = 0;
  int no_run_if_empty = 0;
  int max_args = 0;
  int cmdc;
  int argi = 0;
  char **cmdv;
  char *default_cmd[] = { "echo", 0 };
  int hard_limit;
  int batch_limit;
  char base_line[XARGS_LINE_LIMIT + 1];
  char run_line_buf[XARGS_LINE_LIMIT + 1];
  char token[XARGS_TOKEN_MAX + 1];
  size_t base_len = 0;
  size_t used = 0;
  int batch_count = 0;
  int saw_input = 0;
  int rc;

  rc = parse_options(argc, argv, &argi, &use_nul, &no_run_if_empty, &max_args);
  if(rc > 0)
    return 0;
  if(rc < 0){
    xargs_usage();
    return 1;
  }

  cmdc = argc - argi;
  cmdv = argv + argi;
  if(cmdc <= 0){
    cmdv = default_cmd;
    cmdc = 1;
  }

  hard_limit = XARGS_MAX_ARGS - cmdc;
  if(hard_limit <= 0){
    fprintf(stderr, "xargs: too many command arguments\n");
    return 1;
  }
  batch_limit = (max_args > 0) ? max_args : hard_limit;
  if(batch_limit > hard_limit)
    batch_limit = hard_limit;
  if(batch_limit <= 0){
    fprintf(stderr, "xargs: no room for input arguments\n");
    return 1;
  }

  base_line[0] = 0;
  for(argi = 0; argi < cmdc; argi++){
    if(append_part(base_line, sizeof(base_line), &base_len, cmdv[argi]) != 0){
      fprintf(stderr, "xargs: base command is too long\n");
      return 1;
    }
  }
  memcpy(run_line_buf, base_line, base_len + 1);
  used = base_len;

  for(;;){
    size_t tok_len;
    int r = use_nul ? read_next_token_nul(token, sizeof(token)) : read_next_token_ws(token, sizeof(token));
    if(r < 0){
      fprintf(stderr, "xargs: token too long\n");
      return 1;
    }
    if(r == 0)
      break;

    saw_input = 1;
    tok_len = strlen(token);
    if(base_len + 1 + tok_len > XARGS_LINE_LIMIT){
      fprintf(stderr, "xargs: single argument too long\n");
      return 1;
    }

    if(batch_count > 0 && (batch_count >= batch_limit || used + 1 + tok_len > XARGS_LINE_LIMIT)){
      rc = run_line(run_line_buf);
      if(rc != 0)
        return rc;
      memcpy(run_line_buf, base_line, base_len + 1);
      used = base_len;
      batch_count = 0;
    }

    if(append_part(run_line_buf, sizeof(run_line_buf), &used, token) != 0){
      fprintf(stderr, "xargs: command line too long\n");
      return 1;
    }
    batch_count++;
  }

  if(batch_count > 0)
    return run_line(run_line_buf);
  if(!saw_input && !no_run_if_empty)
    return run_line(base_line);
  return 0;
}
