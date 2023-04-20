// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);
void stealpage(int id);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

void
kinit()
{
  for (int i = 0; i < NCPU; i++) {
    initlock(&kmem[i].lock, "kmem");
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();

  int id = cpuid();
  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);

  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();

  int id = cpuid();
  stealpage(id);

  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if(r)
    kmem[id].freelist = r->next;
  release(&kmem[id].lock);

  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

// steal free pages for cpu which id == id
// select the next cpu with free pages, steal half of pages 
// should be called with 'push_off()'
void
stealpage(int id)
{
  if (kmem[id].freelist) return;

  for(int k = (id+1) % NCPU; k != id; k = (k+1)%NCPU) {
    acquire(&kmem[k].lock);
    if (!kmem[k].freelist) {
      release(&kmem[k].lock);
      continue;
    }

    struct run *p, *q; // 双指针，p指针每轮前进一步，q指针每轮前进两步
    p = q = kmem[k].freelist;
    for (; q->next && q->next->next; p = p->next, q = q->next->next); 
    // 当q->next为空, 空闲页数量为单数,p指向中间空闲页 
    // 当q->next->next为空, 空闲页数量为双数, p指向前半部分空闲页的最后一页

    kmem[id].freelist = kmem[k].freelist;
    kmem[k].freelist = p->next;
    p->next = 0;

    release(&kmem[k].lock);
    break;
  }

}