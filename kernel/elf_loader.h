#ifndef XV6_ELF_LOADER_H
#define XV6_ELF_LOADER_H

#include "types.h"

#define ELFLOADER_MAX_SEGS 8

typedef struct {
  uint32 vaddr;
  uint32 memsz;
  uint8 *mem;
} elf_loaded_seg_t;

typedef struct {
  uint8 *image;
  uint32 image_size;
  elf_loaded_seg_t segs[ELFLOADER_MAX_SEGS];
  int seg_count;
  void *entry;
  uint16 machine;
  uint16 etype;
} elf_image_t;

int elf_load_from_flash(uint32 sector, uint32 sector_count, elf_image_t *out);
int elf_run(elf_image_t *img, int *retv);
void elf_unload(elf_image_t *img);

#endif
