#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern int k_free_heap(void);
extern int k_total_heap(void);

typedef enum {
  UNIT_BYTES = 0,
  UNIT_KIB,
  UNIT_MIB,
  UNIT_GIB,
  UNIT_HUMAN,
} unit_mode_t;

typedef struct {
  unsigned long long total;
  unsigned long long used;
  unsigned long long free;
  unsigned long long available;
  int valid;
} mem_stats_t;

static void usage(void)
{
  (void)fprintf(stderr, "usage: free [-b|-k|-m|-g|-h]\n");
  exit(1);
}

static int read_mem_stats(mem_stats_t *out)
{
  int total_heap;
  int free_heap;
  unsigned long long total_mem;
  unsigned long long free_mem;

  if(out == NULL) {
    return -1;
  }

  total_heap = k_total_heap();
  free_heap = k_free_heap();
  if(free_heap < 0 || total_heap < 0) {
    return -1;
  }
  free_mem = (unsigned long long)free_heap;
  total_mem = (unsigned long long)total_heap;
  if(total_mem < free_mem) {
    return -1;
  }

  /* Heap view: total from allocator capacity, free from allocator current state. */
  out->total = total_mem;
  out->free = free_mem;
  out->available = free_mem;
  out->used = total_mem - free_mem;
  out->valid = 1;
  return 0;
}

static unsigned long long scale_fixed(unsigned long long bytes, unit_mode_t mode)
{
  unsigned int shift = 0;

  switch(mode) {
    case UNIT_BYTES:
      shift = 0;
      break;
    case UNIT_KIB:
      shift = 10;
      break;
    case UNIT_MIB:
      shift = 20;
      break;
    case UNIT_GIB:
      shift = 30;
      break;
    default:
      shift = 10;
      break;
  }
  if(shift == 0) {
    return bytes;
  }
  return bytes >> shift;
}

static void format_human(unsigned long long bytes, char *out, size_t out_len)
{
  const char *suffix = "B";
  unsigned long long whole = bytes;
  unsigned long long frac = 0;
  unsigned int shift = 0;

  if(bytes >= (1ULL << 30)) {
    suffix = "GiB";
    shift = 30;
  } else if(bytes >= (1ULL << 20)) {
    suffix = "MiB";
    shift = 20;
  } else if(bytes >= (1ULL << 10)) {
    suffix = "KiB";
    shift = 10;
  }

  if(shift != 0) {
    unsigned long long unit = 1ULL << shift;
    whole = bytes / unit;
    frac = (bytes % unit) * 10ULL / unit;
    if(frac > 0) {
      (void)snprintf(out, out_len, "%llu.%llu%s", whole, frac, suffix);
      return;
    }
  }

  (void)snprintf(out, out_len, "%llu%s", whole, suffix);
}

static void print_fixed(const mem_stats_t *stats, unit_mode_t mode)
{
  const char *unit = "KiB";
  unsigned long long total;
  unsigned long long used;
  unsigned long long free_mem;
  unsigned long long avail;

  if(mode == UNIT_BYTES) {
    unit = "B";
  } else if(mode == UNIT_MIB) {
    unit = "MiB";
  } else if(mode == UNIT_GIB) {
    unit = "GiB";
  }

  total = scale_fixed(stats->total, mode);
  used = scale_fixed(stats->used, mode);
  free_mem = scale_fixed(stats->free, mode);
  avail = scale_fixed(stats->available, mode);

  (void)printf("              total        used        free   available\n");
  (void)printf("Mem (%-3s): %12llu %11llu %11llu %11llu\n", unit, total, used, free_mem, avail);
}

static void print_human(const mem_stats_t *stats)
{
  char total[32];
  char used[32];
  char free_mem[32];
  char avail[32];

  format_human(stats->total, total, sizeof(total));
  format_human(stats->used, used, sizeof(used));
  format_human(stats->free, free_mem, sizeof(free_mem));
  format_human(stats->available, avail, sizeof(avail));

  (void)printf("              total        used        free   available\n");
  (void)printf("Mem: %14s %11s %11s %11s\n", total, used, free_mem, avail);
}

int main(int argc, char **argv)
{
  int ch;
  unit_mode_t mode = UNIT_KIB;
  mem_stats_t stats;

  while((ch = getopt(argc, argv, "bkmgh")) != -1) {
    switch(ch) {
      case 'b':
        mode = UNIT_BYTES;
        break;
      case 'k':
        mode = UNIT_KIB;
        break;
      case 'm':
        mode = UNIT_MIB;
        break;
      case 'g':
        mode = UNIT_GIB;
        break;
      case 'h':
        mode = UNIT_HUMAN;
        break;
      default:
        usage();
        break;
    }
  }

  if(optind != argc) {
    usage();
  }

  if(read_mem_stats(&stats) != 0 || !stats.valid) {
    (void)fprintf(stderr, "free: failed to query memory stats\n");
    return 1;
  }

  if(mode == UNIT_HUMAN) {
    print_human(&stats);
  } else {
    print_fixed(&stats, mode);
  }
  return 0;
}
