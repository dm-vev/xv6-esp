#include "ksh.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "elf_loader.h"
#include "esp_flash_disk.h"
#include "fs.h"
#include "hal.h"
#include "xv6fs_ro.h"

static int k_ticks(void)
{
  return (int)hal_ticks();
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

static int k_fs_readdir(int index, char *name_out, int name_out_len, uint32 *size_out)
{
  return xv6fs_ro_list(index, name_out, name_out_len, size_out);
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

static void print_hex_u8(uint8 x)
{
  const char hexd[] = "0123456789abcdef";
  putc_console(hexd[(x >> 4) & 0x0f]);
  putc_console(hexd[x & 0x0f]);
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

static uint32 parse_u32(const char *s, int *ok)
{
  uint32 v = 0;
  const char *p = s;

  *ok = 0;
  if(*p == 0)
    return 0;

  while(*p != 0){
    if(*p < '0' || *p > '9')
      return 0;
    v = v * 10u + (uint32)(*p - '0');
    p++;
  }
  *ok = 1;
  return v;
}

static void cmd_help(void)
{
  puts_line("commands:");
  puts_line("  help");
  puts_line("  echo <text>");
  puts_line("  ls");
  puts_line("  mem");
  puts_line("  ticks");
  puts_line("  diskinfo");
  puts_line("  diskread <sector>");
  puts_line("  diskwrite <sector> <byte0-255>");
  puts_line("  reboot");
}

static void cmd_mem(void)
{
  puts_console("free_heap=");
  print_u64(hal_free_heap_bytes());
  puts_line(" bytes");
}

static void cmd_ticks(void)
{
  puts_console("ticks=");
  print_u64(hal_ticks());
  puts_line("");
}

static void cmd_diskinfo(void)
{
  puts_console("disk sectors=");
  print_u32(esp_flash_disk_num_sectors());
  puts_line("");
}

static void cmd_diskread(uint32 sector)
{
  uint8 buf[XV6_FLASH_SECTOR_SIZE];
  int rc = esp_flash_disk_read(sector, buf, 1);
  if(rc != 0){
    puts_line("diskread error");
    return;
  }

  puts_console("sector ");
  print_u32(sector);
  puts_console(": ");
  for(int i = 0; i < 16; i++){
    print_hex_u8(buf[i]);
    putc_console(' ');
  }
  puts_line("");
}

static void cmd_diskwrite(uint32 sector, uint8 value)
{
  uint8 buf[XV6_FLASH_SECTOR_SIZE];

  for(uint32 i = 0; i < XV6_FLASH_SECTOR_SIZE; i++)
    buf[i] = value;

  if(esp_flash_disk_write(sector, buf, 1) != 0){
    puts_line("diskwrite error");
    return;
  }

  puts_line("diskwrite ok");
}

static void cmd_fsls(void)
{
  int i = 0;
  char name[DIRSIZ + 1];
  uint32 size = 0;

  while(xv6fs_ro_list(i, name, sizeof(name), &size) == 0){
    puts_console(name);
    puts_console(" ");
    print_u32(size);
    puts_line("B");
    i++;
  }
  if(i == 0)
    puts_line("fsls: empty");
}

static int try_run_elf_command(int argc, char **argv)
{
  elf_module_t *m;
  void *image = 0;
  uint32 image_size = 0;
  int retv = 0;
  char alt_name[ELFLOADER_NAME_MAX];

  if(argc <= 0 || argv == 0 || argv[0] == 0 || argv[0][0] == 0)
    return -1;

  m = elf_module_find(argv[0]);
  if(m == 0){
    if(xv6fs_ro_read_file_alloc(argv[0], &image, &image_size) != 0){
      size_t n = strlen(argv[0]);
      if(n + 4 >= sizeof(alt_name))
        return -1;
      memcpy(alt_name, argv[0], n);
      memcpy(alt_name + n, ".elf", 5);
      if(xv6fs_ro_read_file_alloc(alt_name, &image, &image_size) != 0){
        puts_line("exec: file not found");
        return -1;
      }
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
    { "xv6fs_readdir", (void *)k_fs_readdir },
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
      int ok;
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
      } else if(strcmp(argv[0], "echo") == 0){
        if(argc >= 2){
          for(int i = 1; i < argc; i++){
            puts_console(argv[i]);
            if(i + 1 < argc)
              putc_console(' ');
          }
          puts_line("");
        } else {
          puts_line("");
        }
      } else if(strcmp(argv[0], "ls") == 0){
        cmd_fsls();
      } else if(strcmp(argv[0], "mem") == 0){
        cmd_mem();
      } else if(strcmp(argv[0], "ticks") == 0){
        cmd_ticks();
      } else if(strcmp(argv[0], "diskinfo") == 0){
        cmd_diskinfo();
      } else if(strcmp(argv[0], "diskread") == 0){
        uint32 sector = (argc >= 2) ? parse_u32(argv[1], &ok) : 0;
        if(argc < 2 || !ok)
          puts_line("usage: diskread <sector>");
        else
          cmd_diskread(sector);
      } else if(strcmp(argv[0], "diskwrite") == 0){
        uint32 sector = (argc >= 2) ? parse_u32(argv[1], &ok) : 0;
        int ok2 = 0;
        uint32 value = (argc >= 3) ? parse_u32(argv[2], &ok2) : 0;
        if(argc < 3 || !ok || !ok2 || value > 255u)
          puts_line("usage: diskwrite <sector> <byte0-255>");
        else
          cmd_diskwrite(sector, (uint8)value);
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
