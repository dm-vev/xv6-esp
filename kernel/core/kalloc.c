/**
 * @file kalloc.c
 * @brief Physical memory allocator implementation.
 *
 * Allocates whole 4096-byte pages for:
 * - User processes
 * - Kernel stacks
 * - Page-table pages
 * - Pipe buffers
 */

#include "core/types.h"
#include "core/param.h"
#include "core/memlayout.h"
#include "core/spinlock.h"
#include "arch/riscv.h"
#include "core/defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

/**
 * @brief Free list node structure.
 *
 * Each free page starts with this structure.
 */
struct run {
  struct run *next;
};

/**
 * @brief Kernel memory allocator state.
 */
struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

/**
 * @brief Initializes the kernel memory allocator.
 *
 * Sets up the free list with all available physical memory
 * from end of kernel to PHYSTOP.
 *
 * @post All physical memory in range is added to free list.
 *
 * @return None.
 */
void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

/**
 * @brief Adds a range of physical memory to the free list.
 *
 * @param pa_start Start of physical memory range.
 * @param pa_end   End of physical memory range.
 *
 * @pre pa_start must be page-aligned.
 *
 * @post All pages in range are added to free list.
 *
 * @return None.
 */
void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

/**
 * @brief Frees a page of physical memory.
 *
 * @param pa Pointer to physical address to free.
 *
 * @pre pa must be page-aligned.
 * @pre pa must be within kernel memory range [end, PHYSTOP).
 *
 * @post Page is added to free list.
 *
 * @note Fills page with junk (0x1) to catch dangling references.
 *
 * @return None.
 *
 * @error Panics if pa is not page-aligned or out of range.
 */
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

/**
 * @brief Allocates one 4096-byte page of physical memory.
 *
 * @post On success, returns pointer to allocated page.
 *
 * @return Pointer to allocated page on success, NULL on failure.
 *
 * @note Fills allocated page with junk (0x5) to catch uninitialized use.
 */
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
