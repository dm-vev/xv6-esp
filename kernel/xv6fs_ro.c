#include "xv6fs_ro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_flash_disk.h"
#include "esp_log.h"
#include "fs.h"
#include "param.h"
#include "stat.h"

static const char *TAG = "xv6fs";

static struct superblock g_sb;
static int g_ready;
static uint32 g_nbitmap;
static uint32 g_data_start;

static int read_block(uint32 bno, void *dst)
{
  return esp_flash_disk_read(bno * (BSIZE / XV6_FLASH_SECTOR_SIZE), dst,
                             (BSIZE / XV6_FLASH_SECTOR_SIZE));
}

static int write_block(uint32 bno, const void *src)
{
  return esp_flash_disk_write(bno * (BSIZE / XV6_FLASH_SECTOR_SIZE), src,
                              (BSIZE / XV6_FLASH_SECTOR_SIZE));
}

static int read_inode(uint32 inum, struct dinode *out)
{
  uint8 blk[BSIZE];
  uint32 bn;
  struct dinode *dip;

  if(out == 0 || !g_ready || inum == 0)
    return -1;
  bn = IBLOCK(inum, g_sb);
  if(read_block(bn, blk) != 0)
    return -1;
  dip = ((struct dinode *)blk) + (inum % IPB);
  *out = *dip;
  return 0;
}

static int write_inode(uint32 inum, const struct dinode *in)
{
  uint8 blk[BSIZE];
  uint32 bn;
  struct dinode *dip;

  if(in == 0 || !g_ready || inum == 0)
    return -1;
  bn = IBLOCK(inum, g_sb);
  if(read_block(bn, blk) != 0)
    return -1;
  dip = ((struct dinode *)blk) + (inum % IPB);
  *dip = *in;
  return write_block(bn, blk);
}

static int inode_data_block(const struct dinode *ip, uint32 fbn, uint32 *out_bno)
{
  uint8 iblk[BSIZE];
  uint32 *indirect = (uint32 *)iblk;

  if(ip == 0 || out_bno == 0)
    return -1;
  if(fbn < NDIRECT){
    *out_bno = ip->addrs[fbn];
    return (*out_bno != 0) ? 0 : -1;
  }
  fbn -= NDIRECT;
  if(fbn >= NINDIRECT || ip->addrs[NDIRECT] == 0)
    return -1;
  if(read_block(ip->addrs[NDIRECT], iblk) != 0)
    return -1;
  *out_bno = indirect[fbn];
  return (*out_bno != 0) ? 0 : -1;
}

static int inode_read_range(const struct dinode *ip, uint32 off, void *dst, uint32 n)
{
  uint8 blk[BSIZE];
  uint8 *out = (uint8 *)dst;
  uint32 copied = 0;

  if(ip == 0 || dst == 0)
    return -1;
  if(off + n > ip->size)
    return -1;

  while(copied < n){
    uint32 file_off = off + copied;
    uint32 fbn = file_off / BSIZE;
    uint32 boff = file_off % BSIZE;
    uint32 take = BSIZE - boff;
    uint32 dblk;

    if(take > (n - copied))
      take = n - copied;
    if(inode_data_block(ip, fbn, &dblk) != 0)
      return -1;
    if(read_block(dblk, blk) != 0)
      return -1;
    memcpy(out + copied, blk + boff, take);
    copied += take;
  }
  return 0;
}

static int bitmap_get(uint32 bno, int *used)
{
  uint8 buf[BSIZE];
  uint32 bb = BBLOCK(bno, g_sb);
  uint32 bi = bno % BPB;
  if(read_block(bb, buf) != 0)
    return -1;
  *used = ((buf[bi / 8] >> (bi % 8)) & 1);
  return 0;
}

static int bitmap_set(uint32 bno, int used)
{
  uint8 buf[BSIZE];
  uint32 bb = BBLOCK(bno, g_sb);
  uint32 bi = bno % BPB;
  if(read_block(bb, buf) != 0)
    return -1;
  if(used)
    buf[bi / 8] |= (1u << (bi % 8));
  else
    buf[bi / 8] &= ~(1u << (bi % 8));
  return write_block(bb, buf);
}

static int alloc_block(uint32 *out_bno)
{
  uint32 b;
  for(b = g_data_start; b < g_sb.size; b++){
    int used = 0;
    if(bitmap_get(b, &used) != 0)
      return -1;
    if(!used){
      uint8 zero[BSIZE];
      memset(zero, 0, sizeof(zero));
      if(bitmap_set(b, 1) != 0)
        return -1;
      if(write_block(b, zero) != 0)
        return -1;
      *out_bno = b;
      return 0;
    }
  }
  return -1;
}

static int free_block(uint32 bno)
{
  return bitmap_set(bno, 0);
}

static int alloc_inode(short type, uint32 *out_inum)
{
  uint32 i;
  for(i = 1; i < g_sb.ninodes; i++){
    struct dinode ip;
    if(read_inode(i, &ip) != 0)
      return -1;
    if(ip.type == 0){
      memset(&ip, 0, sizeof(ip));
      ip.type = type;
      ip.nlink = 1;
      ip.size = 0;
      if(write_inode(i, &ip) != 0)
        return -1;
      *out_inum = i;
      return 0;
    }
  }
  return -1;
}

static int inode_get_or_alloc_block(struct dinode *ip, uint32 fbn, int alloc, uint32 *out_bno)
{
  uint8 iblk[BSIZE];
  uint32 *indirect = (uint32 *)iblk;

  if(fbn < NDIRECT){
    if(ip->addrs[fbn] == 0 && alloc){
      if(alloc_block(&ip->addrs[fbn]) != 0)
        return -1;
    }
    *out_bno = ip->addrs[fbn];
    return (*out_bno != 0) ? 0 : -1;
  }

  fbn -= NDIRECT;
  if(fbn >= NINDIRECT)
    return -1;
  if(ip->addrs[NDIRECT] == 0){
    if(!alloc)
      return -1;
    if(alloc_block(&ip->addrs[NDIRECT]) != 0)
      return -1;
    memset(iblk, 0, sizeof(iblk));
    if(write_block(ip->addrs[NDIRECT], iblk) != 0)
      return -1;
  }
  if(read_block(ip->addrs[NDIRECT], iblk) != 0)
    return -1;
  if(indirect[fbn] == 0 && alloc){
    if(alloc_block(&indirect[fbn]) != 0)
      return -1;
    if(write_block(ip->addrs[NDIRECT], iblk) != 0)
      return -1;
  }
  *out_bno = indirect[fbn];
  return (*out_bno != 0) ? 0 : -1;
}

static int inode_write_range(struct dinode *ip, uint32 off, const void *src, uint32 n)
{
  uint8 blk[BSIZE];
  const uint8 *in = (const uint8 *)src;
  uint32 copied = 0;

  if(ip == 0 || src == 0)
    return -1;

  while(copied < n){
    uint32 file_off = off + copied;
    uint32 fbn = file_off / BSIZE;
    uint32 boff = file_off % BSIZE;
    uint32 take = BSIZE - boff;
    uint32 dblk;

    if(take > (n - copied))
      take = n - copied;
    if(inode_get_or_alloc_block(ip, fbn, 1, &dblk) != 0)
      return -1;
    if(read_block(dblk, blk) != 0)
      return -1;
    memcpy(blk + boff, in + copied, take);
    if(write_block(dblk, blk) != 0)
      return -1;
    copied += take;
  }
  if(off + n > ip->size)
    ip->size = off + n;
  return 0;
}

static int inode_truncate(uint32 inum, struct dinode *ip)
{
  uint32 i;
  uint8 iblk[BSIZE];
  uint32 *indirect = (uint32 *)iblk;

  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      if(free_block(ip->addrs[i]) != 0)
        return -1;
      ip->addrs[i] = 0;
    }
  }
  if(ip->addrs[NDIRECT]){
    if(read_block(ip->addrs[NDIRECT], iblk) != 0)
      return -1;
    for(i = 0; i < NINDIRECT; i++){
      if(indirect[i] && free_block(indirect[i]) != 0)
        return -1;
    }
    if(free_block(ip->addrs[NDIRECT]) != 0)
      return -1;
    ip->addrs[NDIRECT] = 0;
  }
  ip->size = 0;
  return write_inode(inum, ip);
}

static int next_path_elem(const char **pp, char *name)
{
  const char *p = *pp;
  int n = 0;

  while(*p == '/')
    p++;
  if(*p == 0){
    *pp = p;
    return 0;
  }
  while(*p && *p != '/'){
    if(n < DIRSIZ)
      name[n++] = *p;
    p++;
  }
  name[n] = 0;
  while(*p == '/')
    p++;
  *pp = p;
  return 1;
}

static int dir_lookup_inum(uint32 dir_inum, const char *name, uint32 *out_inum, struct dinode *out_ip)
{
  struct dinode dir;
  uint32 off;

  if(read_inode(dir_inum, &dir) != 0 || dir.type != T_DIR)
    return -1;

  for(off = 0; off + sizeof(struct dirent) <= dir.size; off += sizeof(struct dirent)){
    struct dirent de;
    char dname[DIRSIZ + 1];
    if(inode_read_range(&dir, off, &de, sizeof(de)) != 0)
      return -1;
    if(de.inum == 0)
      continue;
    memset(dname, 0, sizeof(dname));
    memcpy(dname, de.name, DIRSIZ);
    if(strcmp(name, dname) == 0){
      if(out_inum)
        *out_inum = de.inum;
      if(out_ip && read_inode(de.inum, out_ip) != 0)
        return -1;
      return 0;
    }
  }
  return 1;
}

static int path_lookup(const char *path, uint32 *out_inum, struct dinode *out_ip)
{
  const char *p = path;
  uint32 inum = ROOTINO;
  struct dinode ip;
  char elem[DIRSIZ + 1];

  if(path == 0 || path[0] == 0)
    return -1;
  if(read_inode(ROOTINO, &ip) != 0)
    return -1;
  if(strcmp(path, "/") == 0){
    if(out_inum)
      *out_inum = ROOTINO;
    if(out_ip)
      *out_ip = ip;
    return 0;
  }

  while(next_path_elem(&p, elem)){
    uint32 next_inum = 0;
    struct dinode next_ip;
    int rc = dir_lookup_inum(inum, elem, &next_inum, &next_ip);
    if(rc != 0)
      return rc;
    inum = next_inum;
    ip = next_ip;
  }

  if(out_inum)
    *out_inum = inum;
  if(out_ip)
    *out_ip = ip;
  return 0;
}

static int path_parent(const char *path, uint32 *parent_inum, char *name_out)
{
  const char *p = path;
  uint32 inum = ROOTINO;
  struct dinode ip;
  char elem[DIRSIZ + 1];
  char next[DIRSIZ + 1];

  if(path == 0 || path[0] == 0 || path[0] != '/')
    return -1;
  if(read_inode(ROOTINO, &ip) != 0)
    return -1;

  if(!next_path_elem(&p, elem))
    return -1;
  while(1){
    const char *save = p;
    if(!next_path_elem(&save, next)){
      if(parent_inum)
        *parent_inum = inum;
      strcpy(name_out, elem);
      return 0;
    }
    {
      uint32 next_inum = 0;
      struct dinode next_ip;
      if(dir_lookup_inum(inum, elem, &next_inum, &next_ip) != 0 || next_ip.type != T_DIR)
        return -1;
      inum = next_inum;
      ip = next_ip;
      (void)ip;
      p = save;
      strcpy(elem, next);
    }
  }
}

static int dir_add_entry(uint32 dir_inum, const char *name, uint32 inum)
{
  struct dinode dir;
  uint32 off;
  struct dirent de;

  if(strlen(name) > DIRSIZ)
    return -1;
  if(read_inode(dir_inum, &dir) != 0 || dir.type != T_DIR)
    return -1;

  for(off = 0; off + sizeof(de) <= dir.size; off += sizeof(de)){
    if(inode_read_range(&dir, off, &de, sizeof(de)) != 0)
      return -1;
    if(de.inum == 0)
      break;
  }

  memset(&de, 0, sizeof(de));
  de.inum = inum;
  memcpy(de.name, name, strlen(name));
  if(inode_write_range(&dir, off, &de, sizeof(de)) != 0)
    return -1;
  return write_inode(dir_inum, &dir);
}

static int dir_find_entry_offset(uint32 dir_inum, const char *name, uint32 *out_off, struct dirent *out_de)
{
  struct dinode dir;
  uint32 off;

  if(read_inode(dir_inum, &dir) != 0 || dir.type != T_DIR)
    return -1;

  for(off = 0; off + sizeof(struct dirent) <= dir.size; off += sizeof(struct dirent)){
    struct dirent de;
    char dname[DIRSIZ + 1];
    if(inode_read_range(&dir, off, &de, sizeof(de)) != 0)
      return -1;
    if(de.inum == 0)
      continue;
    memset(dname, 0, sizeof(dname));
    memcpy(dname, de.name, DIRSIZ);
    if(strcmp(name, dname) == 0){
      if(out_off)
        *out_off = off;
      if(out_de)
        *out_de = de;
      return 0;
    }
  }
  return 1;
}

int xv6fs_ro_init(void)
{
  uint8 blk[BSIZE];

  g_ready = 0;
  memset(&g_sb, 0, sizeof(g_sb));

  if(read_block(1, blk) != 0)
    return -1;
  memcpy(&g_sb, blk, sizeof(g_sb));
  if(g_sb.magic != FSMAGIC){
    ESP_LOGE(TAG, "bad magic: 0x%08x", g_sb.magic);
    return -1;
  }

  g_nbitmap = g_sb.size / BPB + 1;
  g_data_start = g_sb.bmapstart + g_nbitmap;
  g_ready = 1;
  ESP_LOGI(TAG, "mounted: size=%u nblocks=%u ninodes=%u", g_sb.size, g_sb.nblocks, g_sb.ninodes);
  return 0;
}

int xv6fs_ro_flash_image(const uint8 *image, uint32 image_size)
{
  uint32 sectors;
  if(image == 0 || image_size == 0 || (image_size % XV6_FLASH_SECTOR_SIZE) != 0)
    return -1;
  sectors = image_size / XV6_FLASH_SECTOR_SIZE;
  if(sectors > esp_flash_disk_num_sectors())
    return -1;
  return esp_flash_disk_write(0, image, sectors);
}

int xv6fs_list_path(const char *path, int index, char *name_out, int name_out_len, uint16 *type_out, uint32 *size_out)
{
  uint32 dir_inum;
  struct dinode dir;
  uint32 off;
  int seen = 0;

  if(index < 0 || name_out == 0 || name_out_len <= 1 || !g_ready)
    return -1;
  if(path_lookup(path, &dir_inum, &dir) != 0 || dir.type != T_DIR)
    return -1;

  for(off = 0; off + sizeof(struct dirent) <= dir.size; off += sizeof(struct dirent)){
    struct dirent de;
    struct dinode ent;
    char name[DIRSIZ + 1];
    if(inode_read_range(&dir, off, &de, sizeof(de)) != 0)
      return -1;
    if(de.inum == 0)
      continue;
    memset(name, 0, sizeof(name));
    memcpy(name, de.name, DIRSIZ);
    if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;
    if(seen++ != index)
      continue;

    strncpy(name_out, name, name_out_len - 1);
    name_out[name_out_len - 1] = 0;
    if(read_inode(de.inum, &ent) != 0)
      return -1;
    if(type_out)
      *type_out = ent.type;
    if(size_out)
      *size_out = ent.size;
    return 0;
  }
  return -1;
}

int xv6fs_read_file_alloc_path(const char *path, void **out_data, uint32 *out_size)
{
  uint32 inum;
  struct dinode ip;
  void *buf;

  if(path == 0 || out_data == 0 || out_size == 0 || !g_ready)
    return -1;
  if(path_lookup(path, &inum, &ip) != 0 || ip.type != T_FILE)
    return -1;

  buf = malloc(ip.size ? ip.size : 1);
  if(buf == 0)
    return -1;
  if(ip.size > 0 && inode_read_range(&ip, 0, buf, ip.size) != 0){
    free(buf);
    return -1;
  }
  *out_data = buf;
  *out_size = ip.size;
  return 0;
}

int xv6fs_mkdir_path(const char *path)
{
  uint32 pinum;
  uint32 inum;
  struct dinode pip;
  struct dinode newdir;
  char name[DIRSIZ + 1];
  struct dirent de;

  if(path == 0 || !g_ready)
    return -1;
  if(path_lookup(path, &inum, 0) == 0)
    return 0;
  if(path_parent(path, &pinum, name) != 0)
    return -1;
  if(read_inode(pinum, &pip) != 0 || pip.type != T_DIR)
    return -1;
  if(alloc_inode(T_DIR, &inum) != 0)
    return -1;
  if(read_inode(inum, &newdir) != 0)
    return -1;

  memset(&de, 0, sizeof(de));
  de.inum = inum;
  memcpy(de.name, ".", 1);
  if(inode_write_range(&newdir, 0, &de, sizeof(de)) != 0)
    return -1;
  memset(&de, 0, sizeof(de));
  de.inum = pinum;
  memcpy(de.name, "..", 2);
  if(inode_write_range(&newdir, sizeof(de), &de, sizeof(de)) != 0)
    return -1;
  if(write_inode(inum, &newdir) != 0)
    return -1;
  return dir_add_entry(pinum, name, inum);
}

int xv6fs_write_file_path(const char *path, const void *data, uint32 size)
{
  uint32 pinum, inum;
  char name[DIRSIZ + 1];
  struct dinode ip;
  int rc;

  if(path == 0 || data == 0 || !g_ready)
    return -1;
  if(path_parent(path, &pinum, name) != 0)
    return -1;

  rc = dir_lookup_inum(pinum, name, &inum, &ip);
  if(rc == 1){
    if(alloc_inode(T_FILE, &inum) != 0)
      return -1;
    if(read_inode(inum, &ip) != 0)
      return -1;
    if(dir_add_entry(pinum, name, inum) != 0)
      return -1;
  } else if(rc != 0 || ip.type != T_FILE){
    return -1;
  }

  if(inode_truncate(inum, &ip) != 0)
    return -1;
  if(read_inode(inum, &ip) != 0)
    return -1;
  if(size > 0 && inode_write_range(&ip, 0, data, size) != 0)
    return -1;
  return write_inode(inum, &ip);
}

int xv6fs_unlink_path(const char *path)
{
  uint32 pinum, inum, off;
  char name[DIRSIZ + 1];
  struct dirent de;
  struct dinode pip, ip;

  if(path == 0 || !g_ready || strcmp(path, "/") == 0)
    return -1;
  if(path_parent(path, &pinum, name) != 0)
    return -1;
  if(read_inode(pinum, &pip) != 0 || pip.type != T_DIR)
    return -1;
  if(dir_find_entry_offset(pinum, name, &off, &de) != 0)
    return -1;

  inum = de.inum;
  if(read_inode(inum, &ip) != 0)
    return -1;
  if(ip.type != T_FILE)
    return -1;

  memset(&de, 0, sizeof(de));
  if(inode_write_range(&pip, off, &de, sizeof(de)) != 0)
    return -1;
  if(write_inode(pinum, &pip) != 0)
    return -1;

  if(ip.nlink > 0)
    ip.nlink--;
  if(ip.nlink == 0){
    if(inode_truncate(inum, &ip) != 0)
      return -1;
    memset(&ip, 0, sizeof(ip));
    return write_inode(inum, &ip);
  }
  return write_inode(inum, &ip);
}

int xv6fs_ro_list(int index, char *name_out, int name_out_len, uint32 *size_out)
{
  return xv6fs_list_path("/", index, name_out, name_out_len, 0, size_out);
}

int xv6fs_ro_read_file_alloc(const char *name, void **out_data, uint32 *out_size)
{
  char path[MAXPATH];
  if(name == 0)
    return -1;
  if(name[0] == '/')
    return xv6fs_read_file_alloc_path(name, out_data, out_size);
  snprintf(path, sizeof(path), "/%s", name);
  return xv6fs_read_file_alloc_path(path, out_data, out_size);
}
