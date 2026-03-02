#include "runtime/shell_parse.h"

#include <string.h>

int parse_u32_dec(const char *s, uint32 *out)
{
  uint32 v = 0;
  uint32 d;
  if(s == 0 || *s == 0 || out == 0)
    return -1;
  while(*s){
    if(*s < '0' || *s > '9')
      return -1;
    d = (uint32)(*s - '0');
    if(v > 429496729u || (v == 429496729u && d > 5u))
      return -1;
    v = v * 10u + d;
    s++;
  }
  *out = v;
  return 0;
}

int parse_i32_dec(const char *s, int *out)
{
  uint32 v = 0;
  uint32 d;
  int neg = 0;

  if(s == 0 || *s == 0 || out == 0)
    return -1;

  if(*s == '-'){
    neg = 1;
    s++;
    if(*s == 0)
      return -1;
  }

  while(*s){
    if(*s < '0' || *s > '9')
      return -1;
    d = (uint32)(*s - '0');
    if(!neg){
      if(v > 214748364u || (v == 214748364u && d > 7u))
        return -1;
    } else {
      if(v > 214748364u || (v == 214748364u && d > 8u))
        return -1;
    }
    v = v * 10u + d;
    s++;
  }

  if(neg){
    if(v == 2147483648u)
      *out = (-2147483647 - 1);
    else
      *out = -(int)v;
  } else {
    *out = (int)v;
  }
  return 0;
}

int parse_line(char *line, char **argv, int max_args)
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
          return -2;
        argv[argc++] = tok;
        tok = 0;
      }
      continue;
    }

    if(ch == '|' || ch == '&' || ch == '<' || ch == '>'){
      if(tok){
        *dst++ = 0;
        if(argc >= max_args)
          return -2;
        argv[argc++] = tok;
        tok = 0;
      }
      if(argc >= max_args)
        return -2;
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
    if(argc < max_args){
      argv[argc++] = tok;
    } else {
      return -2;
    }
  }

  return argc;
}

int is_fd_token(const char *s, int *out_fd)
{
  if(s && s[0] >= '0' && s[0] <= '2' && s[1] == 0){
    if(out_fd)
      *out_fd = (int)(s[0] - '0');
    return 1;
  }
  return 0;
}

int is_redir_token(const char *s)
{
  return (strcmp(s, "<") == 0 || strcmp(s, ">") == 0 || strcmp(s, ">>") == 0);
}

int is_control_token(const char *s)
{
  return (strcmp(s, "|") == 0 || strcmp(s, "&") == 0 || is_redir_token(s));
}

int find_pipe_pos(int argc, char **argv)
{
  int i;
  for(i = 0; i < argc; i++){
    if(strcmp(argv[i], "|") == 0)
      return i;
  }
  return -1;
}

int build_pipeline(int argc, char **argv, int *starts, int *lens, int max_stages)
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
