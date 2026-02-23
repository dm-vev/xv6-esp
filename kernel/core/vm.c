/**
 * @file vm.c
 * @brief Virtual memory management implementation.
 */

#include "core/param.h"
#include "core/types.h"
#include "core/memlayout.h"
#include "loader/elf.h"
#include "arch/riscv.h"
#include "core/defs.h"
#include "core/spinlock.h"
#include "core/proc.h"
#include "fs/fs.h"

/**
 * @brief The kernel's page table.
 *
 * Direct-mapped page table shared by all CPUs.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

/**
 * @brief Creates a direct-map page table for the kernel.
 *
 * Sets up direct mappings for:
 * - UART registers
 * - Virtio disk interface
 * - PLIC
 * - Kernel text (executable, read-only)
 * - Kernel data and RAM
 * - Trampoline page
 * - Per-process kernel stacks
 *
 * @post Kernel page table is allocated and fully mapped.
 *
 * @return Newly created kernel page table.
 *
 * @error Panics if memory allocation fails during mapping.
 */
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

/**
 * @brief Adds a mapping to the kernel page table.
 *
 * @param kpgtbl Kernel page table to modify.
 * @param va      Virtual address to map.
 * @param pa      Physical address to map.
 * @param sz      Size of mapping in bytes.
 * @param perm    Page table permissions (PTE_R, PTE_W, PTE_X, etc.).
 *
 * @note Only used during boot.
 * @note Does not flush TLB or enable paging.
 *
 * @return None.
 *
 * @error Panics if mappages fails.
 */
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

/**
 * @brief Initializes the kernel page table.
 *
 * Called once during kernel startup, shared by all CPUs.
 *
 * @post kernel_pagetable is set to a new kernel page table.
 *
 * @return None.
 */
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

/**
 * @brief Activates the kernel page table on the current CPU.
 *
 * Switches the hardware page table register to the kernel's page table
 * and enables paging.
 *
 * @post SATP register points to kernel_pagetable.
 * @post Paging is enabled.
 *
 * @note Flushes TLB before and after switching to ensure consistency.
 *
 * @return None.
 */
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

/**
 * @brief Walks the page table to find the PTE for a virtual address.
 *
 * Uses RISC-V Sv39 scheme with three levels of page-table pages.
 * Each page-table page contains 512 64-bit PTEs.
 *
 * @param pagetable Page table to walk.
 * @param va        Virtual address to look up.
 * @param alloc     If non-zero, allocate missing page-table pages.
 *
 * @pre va must be less than MAXVA.
 *
 * @return Pointer to PTE on success, NULL if allocation failed.
 *
 * @error Panics if va >= MAXVA.
 * @error Returns NULL if alloc is true and memory allocation fails.
 */
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

/**
 * @brief Looks up a virtual address and returns the physical address.
 *
 * Only works for user pages (must have PTE_U set).
 *
 * @param pagetable Page table to search.
 * @param va        Virtual address to translate.
 *
 * @pre va must be less than MAXVA.
 *
 * @return Physical address if mapped, 0 otherwise.
 *
 * @note Returns 0 if PTE doesn't exist or isn't valid/user.
 */
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

/**
 * @brief Creates PTEs mapping virtual addresses to physical addresses.
 *
 * @param pagetable Page table to modify.
 * @param va        Starting virtual address (must be page-aligned).
 * @param size      Size of memory to map (must be page-aligned).
 * @param pa        Starting physical address.
 * @param perm      Permissions (PTE_R, PTE_W, PTE_X, PTE_U).
 *
 * @pre va and size must be page-aligned.
 *
 * @return 0 on success, -1 if page table allocation fails.
 *
 * @error Panics if va, size not aligned, or remapping attempted.
 */
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

/**
 * @brief Creates an empty user page table.
 *
 * @post New page table allocated and zeroed.
 *
 * @return New page table on success, NULL if out of memory.
 */
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

/**
 * @brief Removes mappings for a range of virtual addresses.
 *
 * @param pagetable Page table to modify.
 * @param va        Starting virtual address (must be page-aligned).
 * @param npages    Number of pages to unmap.
 * @param do_free   If true, free the underlying physical memory.
 *
 * @pre va must be page-aligned.
 *
 * @post Specified virtual pages are unmapped.
 * @post Physical memory is freed if do_free is true.
 *
 * @note It's OK if mappings don't exist - they will be skipped.
 *
 * @return None.
 */
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
      continue;   
    if((*pte & PTE_V) == 0)  // has physical page been allocated?
      continue;
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

/**
 * @brief Allocates physical memory and maps it into user space.
 *
 * Grows process memory from oldsz to newsz by allocating
 * new pages and mapping them.
 *
 * @param pagetable User page table.
 * @param oldsz     Current size of process memory.
 * @param newsz     Desired new size.
 * @param xperm     Extra permissions (e.g., PTE_X for executable).
 *
 * @pre newsz >= oldsz.
 *
 * @post New pages are allocated and mapped.
 *
 * @return New size on success, 0 on failure.
 */
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

/**
 * @brief Deallocates user memory to shrink process size.
 *
 * @param pagetable User page table.
 * @param oldsz     Current size.
 * @param newsz     Target size (can be larger than oldsz).
 *
 * @post Memory from newsz to oldsz is freed if newsz < oldsz.
 *
 * @return New process size (may be less than newsz if alignment changes).
 */
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

/**
 * @brief Recursively frees all page-table pages.
 *
 * @param pagetable Page table to free.
 *
 * @pre All leaf mappings must have been removed.
 *
 * @post All page-table pages are freed.
 *
 * @error Panics if leaf mappings still exist.
 */
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

/**
 * @brief Frees all user memory and page table.
 *
 * @param pagetable User page table to free.
 * @param sz        Size of user memory in bytes.
 *
 * @post All user pages and page-table pages are freed.
 *
 * @return None.
 */
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

/**
 * @brief Copies parent's address space to child.
 *
 * Copies both page table entries and physical memory.
 *
 * @param old Parent's page table.
 * @param new Child's page table.
 * @param sz  Size of parent process memory.
 *
 * @post Child has copy of parent's memory.
 *
 * @return 0 on success, -1 on failure.
 *
 * @error Frees any allocated pages on failure.
 */
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      continue;   // page table entry hasn't been allocated
    if((*pte & PTE_V) == 0)
      continue;   // physical page hasn't been allocated
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

/**
 * @brief Marks a PTE invalid for user access.
 *
 * Used for user stack guard pages.
 *
 * @param pagetable Page table to modify.
 * @param va        Virtual address to clear PTE_U bit.
 *
 * @post PTE at va no longer has user (PTE_U) permission.
 *
 * @error Panics if PTE doesn't exist.
 *
 * @return None.
 */
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

/**
 * @brief Copies data from kernel to user virtual memory.
 *
 * @param pagetable Page table to use for translation.
 * @param dstva     Destination virtual address.
 * @param src       Source kernel pointer.
 * @param len       Number of bytes to copy.
 *
 * @post Data copied to user virtual address.
 *
 * @return 0 on success, -1 on failure.
 *
 * @error Returns -1 if dstva >= MAXVA.
 * @error Returns -1 if page not writable.
 */
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if(va0 >= MAXVA)
      return -1;
  
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);
    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
      
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

/**
 * @brief Copies data from user to kernel virtual memory.
 *
 * @param pagetable Page table to use for translation.
 * @param dst       Destination kernel pointer.
 * @param srcva     Source virtual address.
 * @param len       Number of bytes to copy.
 *
 * @post Data copied to kernel buffer.
 *
 * @return 0 on success, -1 on failure.
 *
 * @error Returns -1 if translation fails.
 */
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 1)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

/**
 * @brief Copies a null-terminated string from user to kernel.
 *
 * @param pagetable Page table to use for translation.
 * @param dst       Destination kernel buffer.
 * @param srcva     Source virtual address.
 * @param max       Maximum bytes to copy.
 *
 * @post String copied to kernel (null-terminated).
 *
 * @return 0 on success, -1 on failure.
 *
 * @error Returns -1 if no null terminator found within max bytes.
 */
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 1)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

/**
 * @brief Handles page faults for lazily-allocated pages.
 *
 * Allocates and maps user memory for pages that were
 * lazily allocated via sys_sbrk().
 *
 * @param pagetable Page table to modify.
 * @param va        Faulting virtual address.
 * @param read      1 if read fault, 0 if write fault.
 *
 * @pre va must be within process memory bounds.
 *
 * @post New page allocated and mapped if needed.
 *
 * @return Physical address on success, 0 on failure.
 *
 * @note Only allocates if va < p->sz and page not already mapped.
 */
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  struct proc *p = myproc();

  if (va >= p->sz)
    return 0;
  va = PGROUNDDOWN(va);
  if(ismapped(pagetable, va)) {
    return 0;
  }
  mem = (uint64) kalloc();
  if(mem == 0)
    return 0;
  memset((void *) mem, 0, PGSIZE);
  if (mappages(pagetable, va, PGSIZE, mem, PTE_W|PTE_U|PTE_R) != 0) {
    kfree((void *)mem);
    return 0;
  }
  return mem;
}

/**
 * @brief Checks if a virtual address is mapped.
 *
 * @param pagetable Page table to check.
 * @param va        Virtual address.
 *
 * @return 1 if mapped, 0 otherwise.
 */
int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}
