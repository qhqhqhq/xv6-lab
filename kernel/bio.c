// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKETS 13

struct {
  // struct spinlock lock;
  struct buf buf[NBUF];

  struct {
    struct spinlock lock;
    struct buf head;
  } buckets[NBUCKETS];

  // unused buf list
  struct {
    struct spinlock lock;
    struct buf head;
  } unused;
  

  // // Linked list of all buffers, through prev/next.
  // // Sorted by how recently the buffer was used.
  // // head.next is most recent, head.prev is least.
  // struct buf head;
} bcache;

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.unused.lock, "unusedbuf");

  for (int i = 0; i < NBUCKETS; i++) {
    initlock(&bcache.buckets[i].lock, "bcache bucket");

    bcache.buckets[i].head.prev = &bcache.buckets[i].head;
    bcache.buckets[i].head.next = &bcache.buckets[i].head;

  }

  // create linked list of buffers
  bcache.unused.head.prev = &bcache.unused.head;
  bcache.unused.head.next = &bcache.unused.head;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++) {
    b->next = bcache.unused.head.next;
    b->prev = &bcache.unused.head;
    initsleeplock(&b->lock, "buffer");
    bcache.unused.head.next->prev = b;
    bcache.unused.head.next = b;
  }

  // // Create linked list of buffers
  // bcache.head.prev = &bcache.head;
  // bcache.head.next = &bcache.head;
  // for(b = bcache.buf; b < bcache.buf+NBUF; b++){
  //   b->next = bcache.head.next;
  //   b->prev = &bcache.head;
  //   initsleeplock(&b->lock, "buffer");
  //   bcache.head.next->prev = b;
  //   bcache.head.next = b;
  // }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  int hash_index = blockno % NBUCKETS;

  acquire(&bcache.buckets[hash_index].lock);

  // is the block already cached?
  for(b = bcache.buckets[hash_index].head.next; b != &bcache.buckets[hash_index].head; b = b->next) {
    if(b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.buckets[hash_index].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // not cached
  acquire(&bcache.unused.lock);
  b = bcache.unused.head.next;
  if(b) {
    if(b->refcnt != 0) panic("refcnt not zero");

    bcache.unused.head.next = b->next;
    b->next->prev = &bcache.unused.head;

    b->next = bcache.buckets[hash_index].head.next;
    b->next->prev = b;
    bcache.buckets[hash_index].head.next = b;
    b->prev = &bcache.buckets[hash_index].head;

    b->dev = dev;
    b->blockno = blockno;
    b->valid = 0;
    b->refcnt = 1;
    release(&bcache.buckets[hash_index].lock);
    release(&bcache.unused.lock);
    acquiresleep(&b->lock);
    return b;

  } 

  // acquire(&bcache.lock);

  // // Is the block already cached?
  // for(b = bcache.head.next; b != &bcache.head; b = b->next){
  //   if(b->dev == dev && b->blockno == blockno){
  //     b->refcnt++;
  //     release(&bcache.lock);
  //     acquiresleep(&b->lock);
  //     return b;
  //   }
  // }

  // // Not cached.
  // // Recycle the least recently used (LRU) unused buffer.
  // for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
  //   if(b->refcnt == 0) {
  //     b->dev = dev;
  //     b->blockno = blockno;
  //     b->valid = 0;
  //     b->refcnt = 1;
  //     release(&bcache.lock);
  //     acquiresleep(&b->lock);
  //     return b;
  //   }
  // }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  // acquire(&bcache.lock);
  int hash_index = b->blockno % NBUCKETS;
  acquire(&bcache.buckets[hash_index].lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    acquire(&bcache.unused.lock);

    b->next->prev = b->prev;
    b->prev->next = b->next;

    b->next = bcache.unused.head.next;
    b->prev = &bcache.unused.head;
    bcache.unused.head.next->prev = b;
    bcache.unused.head.next = b;

    // // no one is waiting for it.
    // b->next->prev = b->prev;
    // b->prev->next = b->next;
    // b->next = bcache.head.next;
    // b->prev = &bcache.head;
    // bcache.head.next->prev = b;
    // bcache.head.next = b;
    release(&bcache.unused.lock);
  }

  release(&bcache.buckets[hash_index].lock);
  // release(&bcache.lock);
}

void
bpin(struct buf *b) {
  int hash_index = b->blockno % NBUCKETS;
  acquire(&bcache.buckets[hash_index].lock);
  b->refcnt++;
  release(&bcache.buckets[hash_index].lock);
}

void
bunpin(struct buf *b) {
  int hash_index = b->blockno % NBUCKETS;
  acquire(&bcache.buckets[hash_index].lock);
  b->refcnt--;
  release(&bcache.buckets[hash_index].lock);
}


