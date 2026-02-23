/**
 * @file exec.c
 * @brief Exec system call implementation.
 */

#include "core/types.h"
#include "core/param.h"
#include "core/memlayout.h"
#include "arch/riscv.h"
#include "core/spinlock.h"
#include "core/proc.h"
#include "core/defs.h"
#include "loader/elf.h"

static int loadseg(pde_t *, uint64, struct inode *, uint, uint);

/**
 * @brief Converts ELF flags to PTE permissions.
 *
 * @param flags ELF program header flags.
 *
 * @return PTE permission bits (PTE_R, PTE_W, PTE_X).
 */
int flags2perm(int flags)
{
    int perm = 0;
    if(flags & 0x1)
      perm = PTE_X;
    if(flags & 0x2)
      perm |= PTE_W;
    return perm;
}

/**
 * @brief Exec system call implementation.
 *
 * Replaces the current process with a new executable.
 *
 * @param path Path to executable file.
 * @param argv Argument vector for new process.
 *
 * @post Old process memory freed.
 * @post New process memory allocated and loaded.
 * @post Stack set up with arguments.
 *
 * @return 0 on success, -1 on failure.
 *
 * @error Returns -1 if file not found or not executable.
 * @error Returns -1 if ELF format invalid.
 * @error Returns -1 if memory allocation fails.
 */
int
kexec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();

  begin_op();

  // Open the executable file.
  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);

  // Read the ELF header.
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  // Is this really an ELF file?
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Load program into memory.
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    uint64 sz1;
    if((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz, flags2perm(ph.flags))) == 0)
      goto bad;
    sz = sz1;
    if(loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  p = myproc();
  sz = PGROUNDUP(sz);
  uint64 sz1;
  if((sz1 = uvmalloc(pagetable, sz, sz + 2*PGSIZE, PTE_W)) == 0)
    goto bad;
  sz = sz1;
  uvmclear(pagetable, sz - 2*PGSIZE);
  sp = sz;
  stackbase = sp - PGSIZE;

  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++){
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv pointer must be 16-byte aligned
    if(sp < stackbase)
      goto bad;
    int len = strlen(argv[argc]) + 1;
    if(copyout(pagetable, sp, argv[argc], len) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // leave room for trapframe.
  sp -= 16;
  sp -= sp % 16;

  // bind the &curbrk on user stack to the top of the new process's empty page.
  // this gives the kernel a convenient place to find the current break.
  // we could instead just embed this info in the trapframe.
  sp -= sizeof(uint64);
  if(copyout(pagetable, sp, (char*)&p->sz, sizeof(p->sz)) < 0)
    goto bad;

  // push the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char*)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // arguments to user main(argc, argv)
  // argc is returned via the system call return,
  // here we don't return to the caller at all.

  // save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
  
  // commit to the user image.
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
  p->trapframe->epc = elf.entry;  // initial program counter = main
  p->trapframe->sp = sp; // initial stack pointer
  proc_freepagetable(oldpagetable, 0);

  return argc;

 bad:
  if(pagetable)
    proc_freepagetable(pagetable, 0);
  if(ip)
    iunlockput(ip);
  end_op();
  return -1;
}

/**
 * @brief Loads a segment from an ELF file into memory.
 *
 * @param pgdir    Page directory.
 * @param va       Virtual address to load into.
 * @param ip       Inode containing ELF file.
 * @param offset   Offset in inode to read from.
 * @param sz       Size of segment to load.
 *
 * @post Memory allocated and data loaded.
 *
 * @return 0 on success, -1 on failure.
 */
static int
loadseg(pde_t *pgdir, uint64 va, struct inode *ip, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;
  
  for(i = 0; i < sz; i += PGSIZE){
    pa = walkaddr(pgdir, va + i);
    if(pa == 0)
      panic("loadseg: address should exist");
    n = PGSIZE;
    if(i + n > sz)
      n = sz - i;
    if(readi(ip, 0, (uint64)pa, offset + i, n) != n)
      return -1;
  }
  return 0;
}
