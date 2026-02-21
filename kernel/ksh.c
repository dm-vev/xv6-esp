#include "ksh.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
  puts_line("  reboot");
}

static int try_run_elf_command(int argc, char **argv)
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
      puts_line("exec: command not found");
      return -1;
    }
    if(elf_module_load_from_bytes(argv[0], image, image_size, &m) != 0){
      free(image);
      puts_line("exec: elf load failed");
      return -1;
    }
    free(image);
  }

  if(elf_module_call_main(m, argc, argv, &retv) != 0){
    puts_line("exec: entry call failed");
    return -1;
  }
  if(retv != 0){
    puts_console(argv[0]);
    puts_console(": exit=");
    print_u32((uint32)retv);
    puts_line("");
  }
  return 0;
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
    { "xv6fs_write_file_path", (void *)xv6fs_write_file_path },
    { "xv6fs_mkdir_path", (void *)xv6fs_mkdir_path },
  };
  (void)elf_loader_register_host_symbols(syms, (int)(sizeof(syms) / sizeof(syms[0])));
}

void ksh_run(void)
{
  char line[256];
  int len = 0;

  elf_loader_init();
  register_default_symbols();

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
      line[len] = 0;
      puts_line("");

      argc = split(line, argv, 16);
      if(argc == 0){
        puts_console("xv6> ");
        len = 0;
        continue;
      }

      if(strcmp(argv[0], "help") == 0){
        cmd_help();
      } else if(strcmp(argv[0], "reboot") == 0){
        puts_line("rebooting...");
        hal_reboot();
      } else {
        if(try_run_elf_command(argc, argv) != 0)
          puts_line("unknown command");
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
