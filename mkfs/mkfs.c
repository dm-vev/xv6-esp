#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <dirent.h>
#include <sys/stat.h>

#define stat xv6_stat  // avoid clash with host struct stat in kernel headers
#include "kernel/types.h"
#define dirent xv6_dirent
#include "kernel/fs.h"
#undef dirent
#include "kernel/stat.h"
#include "kernel/param.h"
#undef stat

#ifndef static_assert
#define static_assert(a, b) do { switch (0) case 0: case (a): ; } while (0)
#endif
#define min(a, b) ((a) < (b) ? (a) : (b))

#define NINODES 200

// Disk layout:
// [ boot block | sb block | log | inode blocks | free bit map | data blocks ]

int fs_nblocks = FSSIZE;
int nbitmap = FSSIZE/BPB + 1;
int ninodeblocks = NINODES / IPB + 1;
int nlog = LOGBLOCKS+1;   // Header followed by LOGBLOCKS data blocks.
int nmeta;    // Number of meta blocks (boot, sb, nlog, inode, bitmap)
int ndatablocks;  // Number of data blocks

int fsfd;
struct superblock sb;
char zeroes[BSIZE];
uint freeinode = 1;
uint freeblock;


void balloc(int);
void wsect(uint, void*);
void winode(uint, struct dinode*);
void rinode(uint inum, struct dinode *ip);
void rsect(uint sec, void *buf);
uint ialloc(ushort type);
void iappend(uint inum, void *p, int n);
void die(const char *);
int import_path(uint parent_inum, const char *src_path, const char *dst_name);
int import_dir_recursive(uint parent_inum, const char *src_dir, const char *dst_name);
int dir_find(uint dir_inum, const char *name, uint *out_inum);
int dir_add(uint dir_inum, const char *name, uint inum);
uint mkdir_in_fs(uint parent_inum, const char *name);
uint create_file_in_fs(uint parent_inum, const char *name);

// convert to riscv byte order
ushort
xshort(ushort x)
{
  ushort y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  return y;
}

uint
xint(uint x)
{
  uint y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  a[2] = x >> 16;
  a[3] = x >> 24;
  return y;
}

int
main(int argc, char *argv[])
{
  int i;
  uint rootino, off;
  struct xv6_dirent de;
  char buf[BSIZE];
  struct dinode din;
  int argi = 2;


  static_assert(sizeof(int) == 4, "Integers must be 4 bytes!");

  if(argc < 2){
    fprintf(stderr, "Usage: mkfs fs.img [-s blocks] files...\n");
    exit(1);
  }

  if(argc >= 4 && strcmp(argv[2], "-s") == 0){
    fs_nblocks = atoi(argv[3]);
    if(fs_nblocks <= 0){
      fprintf(stderr, "mkfs: invalid -s value: %s\n", argv[3]);
      exit(1);
    }
    argi = 4;
  }

  assert((BSIZE % sizeof(struct dinode)) == 0);
  assert((BSIZE % sizeof(struct xv6_dirent)) == 0);

  fsfd = open(argv[1], O_RDWR|O_CREAT|O_TRUNC, 0666);
  if(fsfd < 0)
    die(argv[1]);

  // 1 fs block = 1 disk sector
  nbitmap = fs_nblocks / BPB + 1;
  nmeta = 2 + nlog + ninodeblocks + nbitmap;
  ndatablocks = fs_nblocks - nmeta;
  if(ndatablocks <= 0){
    fprintf(stderr, "mkfs: fs is too small (%d blocks)\n", fs_nblocks);
    exit(1);
  }

  sb.magic = FSMAGIC;
  sb.size = xint(fs_nblocks);
  sb.nblocks = xint(ndatablocks);
  sb.ninodes = xint(NINODES);
  sb.nlog = xint(nlog);
  sb.logstart = xint(2);
  sb.inodestart = xint(2+nlog);
  sb.bmapstart = xint(2+nlog+ninodeblocks);

  printf("nmeta %d (boot, super, log blocks %u, inode blocks %u, bitmap blocks %u) blocks %d total %d\n",
         nmeta, nlog, ninodeblocks, nbitmap, ndatablocks, fs_nblocks);

  freeblock = nmeta;     // the first free block that we can allocate

  for(i = 0; i < fs_nblocks; i++)
    wsect(i, zeroes);

  memset(buf, 0, sizeof(buf));
  memmove(buf, &sb, sizeof(sb));
  wsect(1, buf);

  rootino = ialloc(T_DIR);
  assert(rootino == ROOTINO);

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, ".");
  iappend(rootino, &de, sizeof(de));

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, "..");
  iappend(rootino, &de, sizeof(de));

  for(i = argi; i < argc; i++){
    const char *src = argv[i];
    const char *name = rindex(src, '/');
    if(name)
      name++;
    else
      name = src;
    if(name[0] == 0)
      continue;
    if(import_path(rootino, src, name) != 0){
      fprintf(stderr, "mkfs: failed to import %s\n", src);
      exit(1);
    }
  }

  // fix size of root inode dir
  rinode(rootino, &din);
  off = xint(din.size);
  off = ((off/BSIZE) + 1) * BSIZE;
  din.size = xint(off);
  winode(rootino, &din);

  balloc(freeblock);

  exit(0);
}

static int
read_inode_at(struct dinode *ip, uint off, void *dst, int n)
{
  uint fbn, n1, x;
  uchar buf[BSIZE];
  uint indirect[NINDIRECT];
  uchar *p = (uchar*)dst;
  uint sz = xint(ip->size);

  if(off > sz || off + n > sz)
    return -1;

  while(n > 0){
    fbn = off / BSIZE;
    if(fbn < NDIRECT){
      x = xint(ip->addrs[fbn]);
    } else {
      if(xint(ip->addrs[NDIRECT]) == 0)
        return -1;
      rsect(xint(ip->addrs[NDIRECT]), (char*)indirect);
      x = xint(indirect[fbn - NDIRECT]);
    }
    if(x == 0)
      return -1;
    rsect(x, buf);
    n1 = min(n, (int)(BSIZE - (off % BSIZE)));
    memmove(p, buf + (off % BSIZE), n1);
    n -= n1;
    p += n1;
    off += n1;
  }
  return 0;
}

int
dir_find(uint dir_inum, const char *name, uint *out_inum)
{
  struct dinode din;
  uint off;
    struct xv6_dirent de;
  char nbuf[DIRSIZ + 1];

  if(out_inum)
    *out_inum = 0;
  if(strlen(name) > DIRSIZ)
    return -1;
  rinode(dir_inum, &din);
  if(xshort(din.type) != T_DIR)
    return -1;

  for(off = 0; off + sizeof(de) <= xint(din.size); off += sizeof(de)){
    if(read_inode_at(&din, off, &de, sizeof(de)) != 0)
      return -1;
    if(de.inum == 0)
      continue;
    memset(nbuf, 0, sizeof(nbuf));
    memmove(nbuf, de.name, DIRSIZ);
    if(strncmp(name, nbuf, DIRSIZ) == 0){
      if(out_inum)
        *out_inum = xshort(de.inum);
      return 0;
    }
  }
  return 1;
}

int
dir_add(uint dir_inum, const char *name, uint inum)
{
  struct xv6_dirent de;
  if(strlen(name) > DIRSIZ)
    return -1;
  bzero(&de, sizeof(de));
  de.inum = xshort(inum);
  strncpy(de.name, name, DIRSIZ);
  iappend(dir_inum, &de, sizeof(de));
  return 0;
}

uint
mkdir_in_fs(uint parent_inum, const char *name)
{
  uint inum = ialloc(T_DIR);
  struct xv6_dirent de;

  bzero(&de, sizeof(de));
  de.inum = xshort(inum);
  strcpy(de.name, ".");
  iappend(inum, &de, sizeof(de));

  bzero(&de, sizeof(de));
  de.inum = xshort(parent_inum);
  strcpy(de.name, "..");
  iappend(inum, &de, sizeof(de));

  if(dir_add(parent_inum, name, inum) != 0)
    return 0;
  return inum;
}

uint
create_file_in_fs(uint parent_inum, const char *name)
{
  uint inum = ialloc(T_FILE);
  if(dir_add(parent_inum, name, inum) != 0)
    return 0;
  return inum;
}

static int
import_file_to_dir(uint parent_inum, const char *src_path, const char *name)
{
  int fd, cc;
  uint inum;
  char buf[BSIZE];

  if(strlen(name) > DIRSIZ)
    return -1;

  fd = open(src_path, 0);
  if(fd < 0)
    return -1;

  inum = create_file_in_fs(parent_inum, name);
  if(inum == 0){
    close(fd);
    return -1;
  }

  while((cc = read(fd, buf, sizeof(buf))) > 0)
    iappend(inum, buf, cc);

  close(fd);
  return 0;
}

int
import_dir_recursive(uint parent_inum, const char *src_dir, const char *dst_name)
{
  DIR *d;
  struct dirent *ent;
  char child_src[1024];
  struct stat st;
  uint dir_inum;
  uint found = 0;

  if(strlen(dst_name) > DIRSIZ)
    return -1;
  if(dir_find(parent_inum, dst_name, &found) == 0){
    dir_inum = found;
  } else {
    dir_inum = mkdir_in_fs(parent_inum, dst_name);
  }
  if(dir_inum == 0)
    return -1;

  d = opendir(src_dir);
  if(d == 0)
    return -1;

  while((ent = readdir(d)) != 0){
    if(strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;
    snprintf(child_src, sizeof(child_src), "%s/%s", src_dir, ent->d_name);
    if(stat(child_src, &st) != 0){
      closedir(d);
      return -1;
    }
    if(S_ISDIR(st.st_mode)){
      if(import_dir_recursive(dir_inum, child_src, ent->d_name) != 0){
        closedir(d);
        return -1;
      }
    } else if(S_ISREG(st.st_mode)){
      if(import_file_to_dir(dir_inum, child_src, ent->d_name) != 0){
        closedir(d);
        return -1;
      }
    }
  }

  closedir(d);
  return 0;
}

int
import_path(uint parent_inum, const char *src_path, const char *dst_name)
{
  struct stat st;
  if(stat(src_path, &st) != 0)
    return -1;

  if(S_ISDIR(st.st_mode))
    return import_dir_recursive(parent_inum, src_path, dst_name);
  if(S_ISREG(st.st_mode))
    return import_file_to_dir(parent_inum, src_path, dst_name);
  return -1;
}

void
wsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE)
    die("lseek");
  if(write(fsfd, buf, BSIZE) != BSIZE)
    die("write");
}

void
winode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *dip = *ip;
  wsect(bn, buf);
}

void
rinode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *ip = *dip;
}

void
rsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE)
    die("lseek");
  if(read(fsfd, buf, BSIZE) != BSIZE)
    die("read");
}

uint
ialloc(ushort type)
{
  uint inum = freeinode++;
  struct dinode din;

  bzero(&din, sizeof(din));
  din.type = xshort(type);
  din.nlink = xshort(1);
  din.size = xint(0);
  winode(inum, &din);
  return inum;
}

void
balloc(int used)
{
  uchar buf[BSIZE];
  int i;

  printf("balloc: first %d blocks have been allocated\n", used);
  assert(used < BPB);
  bzero(buf, BSIZE);
  for(i = 0; i < used; i++){
    buf[i/8] = buf[i/8] | (0x1 << (i%8));
  }
  printf("balloc: write bitmap block at sector %d\n", sb.bmapstart);
  wsect(sb.bmapstart, buf);
}

void
iappend(uint inum, void *xp, int n)
{
  char *p = (char*)xp;
  uint fbn, off, n1;
  struct dinode din;
  char buf[BSIZE];
  uint indirect[NINDIRECT];
  uint x;

  rinode(inum, &din);
  off = xint(din.size);
  // printf("append inum %d at off %d sz %d\n", inum, off, n);
  while(n > 0){
    fbn = off / BSIZE;
    assert(fbn < MAXFILE);
    if(fbn < NDIRECT){
      if(xint(din.addrs[fbn]) == 0){
        din.addrs[fbn] = xint(freeblock++);
      }
      x = xint(din.addrs[fbn]);
    } else {
      if(xint(din.addrs[NDIRECT]) == 0){
        din.addrs[NDIRECT] = xint(freeblock++);
      }
      rsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      if(indirect[fbn - NDIRECT] == 0){
        indirect[fbn - NDIRECT] = xint(freeblock++);
        wsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      }
      x = xint(indirect[fbn-NDIRECT]);
    }
    n1 = min(n, (fbn + 1) * BSIZE - off);
    rsect(x, buf);
    bcopy(p, buf + off - (fbn * BSIZE), n1);
    wsect(x, buf);
    n -= n1;
    off += n1;
    p += n1;
  }
  din.size = xint(off);
  winode(inum, &din);
}

void
die(const char *s)
{
  perror(s);
  exit(1);
}
