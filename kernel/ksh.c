#include "ksh.h"

#include <string.h>

#include "esp_flash_disk.h"
#include "hal.h"

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
  const char hexd[16] = "0123456789abcdef";
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

void ksh_run(void)
{
  char line[128];
  int len = 0;

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
      char *argv[4];
      int argc;
      int ok;
      line[len] = 0;
      puts_line("");

      argc = split(line, argv, 4);
      if(argc == 0){
        puts_console("xv6> ");
        len = 0;
        continue;
      }

      if(strcmp(argv[0], "help") == 0){
        cmd_help();
      } else if(strcmp(argv[0], "echo") == 0){
        if(argc >= 2){
          puts_line(argv[1]);
        } else {
          puts_line("");
        }
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
