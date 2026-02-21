#include "elf_loader.h"

#include <string.h>
#include <stdlib.h>

#include "esp_flash_disk.h"
#include "esp_heap_caps.h"

#define ELF_MAGIC 0x464c457fU
#define ELFCLASS32 1
#define ELFDATA2LSB 1

#define ET_EXEC 2
#define ET_DYN 3

#define EM_XTENSA 94

#define PT_LOAD 1
#define XV6_ELF_RUN_ENABLED 0

typedef struct __attribute__((packed)) {
  uint8 e_ident[16];
  uint16 e_type;
  uint16 e_machine;
  uint32 e_version;
  uint32 e_entry;
  uint32 e_phoff;
  uint32 e_shoff;
  uint32 e_flags;
  uint16 e_ehsize;
  uint16 e_phentsize;
  uint16 e_phnum;
  uint16 e_shentsize;
  uint16 e_shnum;
  uint16 e_shstrndx;
} elf32_ehdr_t;

typedef struct __attribute__((packed)) {
  uint32 p_type;
  uint32 p_offset;
  uint32 p_vaddr;
  uint32 p_paddr;
  uint32 p_filesz;
  uint32 p_memsz;
  uint32 p_flags;
  uint32 p_align;
} elf32_phdr_t;

static int load_image_from_flash(uint32 sector, uint32 sector_count, uint8 **out, uint32 *out_sz)
{
  uint32 sz = sector_count * XV6_FLASH_SECTOR_SIZE;
  uint8 *buf;

  if(sector_count == 0 || out == 0 || out_sz == 0)
    return -1;

  buf = (uint8 *)heap_caps_malloc(sz, MALLOC_CAP_8BIT);
  if(buf == 0)
    return -1;

  if(esp_flash_disk_read(sector, buf, sector_count) != 0){
    free(buf);
    return -1;
  }

  *out = buf;
  *out_sz = sz;
  return 0;
}

static int parse_loadable_segments(const uint8 *image, uint32 image_sz, const elf32_ehdr_t *eh, elf_image_t *out)
{
  uint32 i;

  if(eh->e_phnum == 0)
    return -1;
  if(eh->e_phoff + (uint32)eh->e_phnum * (uint32)eh->e_phentsize > image_sz)
    return -1;
  if(eh->e_phentsize < sizeof(elf32_phdr_t))
    return -1;

  for(i = 0; i < eh->e_phnum; i++){
    const uint32 off = eh->e_phoff + i * eh->e_phentsize;
    const elf32_phdr_t *ph = (const elf32_phdr_t *)(image + off);
    uint8 *segmem;

    if(ph->p_type != PT_LOAD)
      continue;
    if(out->seg_count >= ELFLOADER_MAX_SEGS)
      return -1;
    if(ph->p_memsz == 0)
      continue;
    if(ph->p_offset > image_sz || ph->p_filesz > image_sz || ph->p_offset + ph->p_filesz > image_sz)
      return -1;
    if(ph->p_filesz > ph->p_memsz)
      return -1;

    segmem = (uint8 *)heap_caps_malloc(ph->p_memsz, MALLOC_CAP_8BIT);
    if(segmem == 0)
      return -1;

    memset(segmem, 0, ph->p_memsz);
    memcpy(segmem, image + ph->p_offset, ph->p_filesz);

    out->segs[out->seg_count].vaddr = ph->p_vaddr;
    out->segs[out->seg_count].memsz = ph->p_memsz;
    out->segs[out->seg_count].mem = segmem;
    out->seg_count++;
  }

  return (out->seg_count > 0) ? 0 : -1;
}

static void *resolve_entry(const elf_image_t *img, uint32 entry_vaddr)
{
  int i;
  for(i = 0; i < img->seg_count; i++){
    const elf_loaded_seg_t *s = &img->segs[i];
    if(entry_vaddr >= s->vaddr && entry_vaddr < s->vaddr + s->memsz){
      return (void *)(s->mem + (entry_vaddr - s->vaddr));
    }
  }
  return 0;
}

int elf_load_from_flash(uint32 sector, uint32 sector_count, elf_image_t *out)
{
  elf32_ehdr_t *eh;
  uint8 *image = 0;
  uint32 image_sz = 0;

  if(out == 0)
    return -1;

  memset(out, 0, sizeof(*out));

  if(load_image_from_flash(sector, sector_count, &image, &image_sz) != 0)
    return -1;

  if(image_sz < sizeof(elf32_ehdr_t)){
    free(image);
    return -1;
  }

  eh = (elf32_ehdr_t *)image;
  {
    uint32 magic = 0;
    memcpy(&magic, &eh->e_ident[0], sizeof(magic));
    if(magic != ELF_MAGIC){
      free(image);
      return -1;
    }
  }
  if(eh->e_ident[4] != ELFCLASS32 || eh->e_ident[5] != ELFDATA2LSB){
    free(image);
    return -1;
  }
  if(eh->e_machine != EM_XTENSA){
    free(image);
    return -1;
  }
  if(eh->e_type != ET_EXEC && eh->e_type != ET_DYN){
    free(image);
    return -1;
  }

  out->image = image;
  out->image_size = image_sz;
  out->machine = eh->e_machine;
  out->etype = eh->e_type;

  if(parse_loadable_segments(image, image_sz, eh, out) != 0){
    elf_unload(out);
    return -1;
  }

  out->entry = resolve_entry(out, eh->e_entry);
  if(out->entry == 0){
    elf_unload(out);
    return -1;
  }

  return 0;
}

int elf_run(elf_image_t *img, int *retv)
{
#if XV6_ELF_RUN_ENABLED
  typedef int (*elf_entry_fn_t)(void);
  elf_entry_fn_t fn;
#endif

  if(img == 0 || img->entry == 0)
    return -1;

#if XV6_ELF_RUN_ENABLED
  fn = (elf_entry_fn_t)img->entry;
  if(retv)
    *retv = fn();
  else
    (void)fn();
  return 0;
#else
  (void)retv;
  return -1;
#endif
}

void elf_unload(elf_image_t *img)
{
  int i;
  if(img == 0)
    return;

  for(i = 0; i < img->seg_count; i++){
    if(img->segs[i].mem){
      free(img->segs[i].mem);
      img->segs[i].mem = 0;
    }
  }

  if(img->image){
    free(img->image);
    img->image = 0;
  }
  img->seg_count = 0;
  img->image_size = 0;
  img->entry = 0;
}
