#include "filesys/cache.h"
#include "threads/thread.h"
#include "devices/timer.h"
#include "filesys/filesys.h"
#include "threads/synch.h"
#include <list.h>
#include <string.h>


enum buf_flag_t {
  B_VALID = 0x0, // 00
  B_BUSY = 0x1, // 01
  B_DIRTY = 0x2, // 10
  B_LOADOK = 0x4
};


/* Cache Entry
 */

struct cache_e
{
  struct lock cache_lock; // lock
  block_sector_t sec;
  enum buf_flag_t flag; // TODO: Valid? Invalid?
  uint8_t data[BLOCK_SECTOR_SIZE];
  struct list_elem elem;
};

static struct list cache;

// Only used for element of cache
static struct cache_e _cache_buffer[MAX_CACHE_SIZE];


/*
 * cacheWriteBackThread
 *
 * DESC | Write all dirty data per interval. (Clock algorithm)
 *
 * IN   | aux - Dummy NULL pointer
 *
 */
static void cacheWriteBackThread(void* aux UNUSED)
{
  int i;
  while(true){
    timer_sleep(TIMER_FREQ * 10); // TODO: HOW MUCH?
    // since list_elem 'could' rearrange each time, we just use array.
    
    for(i = 0 ; i < MAX_CACHE_SIZE ; i++)    
      if(_cache_buffer[i].flag & B_DIRTY){ 
        if(!lock_try_acquire(&_cache_buffer[i].cache_lock))
          continue; // is now working?

        block_write(fs_device, _cache_buffer[i].sec, _cache_buffer[i].data);
        _cache_buffer[i].flag -= B_DIRTY;

        lock_release(&_cache_buffer[i].cache_lock);
      }
  }
}

/* 
 * cache_init
 *
 * DESC | Initialize cache in first time.
 *
 */
void cache_init()
{
  int i;
  list_init(&cache);
  for(i = 0 ; i < MAX_CACHE_SIZE ; i++){
    lock_init(&_cache_buffer[i].cache_lock);
    list_push_back(&cache, &_cache_buffer[i].elem);
  }
  thread_create("cache_wb", PRI_DEFAULT, cacheWriteBackThread, NULL);
}

/*
 * cacheGetFree
 *
 * DESC | Get B_VALID cache element
 *
 * RET  | if cache is full, NULL
 *      | else last element of (LOCKED) cache mem
 */
static struct cache_e* 
cacheGetFree(void)
{
  int i;
  for(i = 0 ; i < MAX_CACHE_SIZE ; i++){
    if(_cache_buffer[i].flag
        || !lock_try_acquire(&_cache_buffer[i].cache_lock)) continue;
    _cache_buffer[i].flag |= B_BUSY;
    return _cache_buffer + i;
  }
  return NULL;
}

/*
 * cacheUpdate
 *
 * DESC | Move MRU value to first slot.
 *
 * IN   | e - value that just updated (_ -> !B_VALID)
 *
 */
static void
cacheUpdate(struct list_elem* e)
{
  list_remove(e);
  list_push_front(&cache, e);
}



/*
 * cacheGetIdx
 *
 * DESC | Get element which has same values with given
 *
 * IN   | sec - given sector number
 *
 * RET  | If fail, NULL
 *      | else, list_elem* of found (LOCKED) value
 */
static struct cache_e*
cacheGetIdx(block_sector_t sec)
{
  int i;

  for(i = 0 ; i < MAX_CACHE_SIZE ; i++){
    struct cache_e* temp = _cache_buffer + i;

    if(temp->sec == sec){
      lock_acquire(&temp->cache_lock);
      if(temp->sec != sec){
        lock_release(&temp->cache_lock);
        return NULL;
      }
      else{
        cacheUpdate(&temp->elem);
        return temp;
      }
    }
  }
  return NULL;
}

struct ahead_set
{
  block_sector_t sec;
  struct semaphore* sema;
};


static void cache_eviction(void);


static void cacheLoadThread(void* aux)
{
  struct ahead_set aheadWrap = *(struct ahead_set*)aux;
  free(aux);

  struct cache_e* ahead;


  if(ahead = cacheGetIdx(aheadWrap.sec)){
    lock_release(&ahead->cache_lock);
    sema_up(aheadWrap.sema);
    thread_exit();
  }
  
  while((ahead = cacheGetFree()) == NULL)
    cache_eviction();

  // get lock by cacheGetFree
  //
  ahead->sec = aheadWrap.sec;
  sema_up(aheadWrap.sema);
  
//  if(ahead->flag & B_LOADOK) ahead->flag -= B_LOADOK;

  block_read(fs_device, aheadWrap.sec, ahead->data);
  cacheUpdate(&ahead->elem); 

//  ahead->flag |= B_LOADOK;

  lock_release(&ahead->cache_lock);

  thread_exit();
}

/* 
 * cacheLoadBlock
 *
 * DESC | Load 2 data (Read-Ahead) from sector if cache miss occur, 
 *      | and return list_elem* of sector
 *
 * IN   | sec - given sector number
 *
 * RET  | list_elem* of given value
 *
 */


static struct cache_e*
cacheLoadBlock(block_sector_t sec)
{
  struct semaphore sema1;
  sema_init(&sema1, 0);

  struct cache_e* ndata;
  while((ndata = cacheGetFree()) == NULL)
    cache_eviction(); 

    // get lock by cacheGetFree
  //
 
//  if(ndata->flag & B_LOADOK) ndata->flag -= B_LOADOK;
  ndata->sec = sec;

  block_read(fs_device, sec, ndata->data);
//  ndata->flag |= B_LOADOK;
  cacheUpdate(&ndata->elem);

  struct ahead_set* aheadWrap = malloc(sizeof(struct ahead_set));

 // block_sector_t *ahead_sec = malloc(sizeof(block_sector_t));
  if(aheadWrap){
    aheadWrap->sec = sec + 1;
    aheadWrap->sema = &sema1;
    thread_create("ahead_reader", PRI_DEFAULT, cacheLoadThread, aheadWrap);
    sema_down(&sema1);
  }
 
  // still get lock
  return ndata;
}

/* NOTE:
 * every function that use read/write function will take 
 * cache_e size >= BLOCK_SECTOR_SIZE with bounce.
 * Do not have to care about cache_e size. 
 */


/*
 * cache_write
 *
 * DESC | Write Data 'from' and other extra field to valid cache.
 *      | Then, Update Cache(MRU), mark (BUSY | DIRTY) flag.
 *
 * IN   | sec - given sector number
 *      | from - caller's data
 *
 */
void cache_write(block_sector_t sec, const void* from)
{
  struct cache_e* buffer = cacheGetIdx(sec);
  if(buffer == NULL)
    buffer = cacheLoadBlock(sec);

  // get lock by cacheGetIdx or cacheLoadBlock

  memcpy(buffer->data, from, BLOCK_SECTOR_SIZE);
  buffer->flag |= B_DIRTY;
  lock_release(&buffer->cache_lock);
}


/*
 * cache_read
 *
 * DESC | Write cache to Data 'to', and fill extra field to valid cache.
 *      | Then, Update Cache(MRU), mark BUSY flag.
 *
 * IN   | sec - given sector number
 *      | to - caller's data
 *
 */
void cache_read(block_sector_t sec, void* to)
{
  struct cache_e* buffer = cacheGetIdx(sec);
  if(buffer == NULL)
    buffer = cacheLoadBlock(sec);

  // get lock by cacheGetIdx or cacheLoadBlock
  
  memcpy(to, buffer->data, BLOCK_SECTOR_SIZE);
  lock_release(&buffer->cache_lock);
}



/*
 * oneblock_init
 *
 * DESC | Init buffer to empty
 *
 * IN   | buffer - given buffer
 *
 */
static void oneblock_release(struct cache_e* buffer)
{
  buffer->sec = 0;
  buffer->flag = B_VALID;
//  memset(buffer->data, 0, BLOCK_SECTOR_SIZE); // Really have to do?
}


/*
 * cache_force_one
 *
 * DESC | force one sector of cache to write-back, and initialize.
 *      | "force clear"
 *
 * IN   | pos - list_elem* of cache
 *
 */
static void cache_force_one(struct cache_e* buffer)
{
  if(buffer->flag & B_DIRTY){
    buffer->flag -= B_DIRTY;
    block_write(fs_device, buffer->sec, buffer->data);
  }
  oneblock_release(buffer);
}

/*
 * cache_flush
 *
 * DESC | clear all cache (If dirty, write-back)
 *
 */
void cache_flush(void)
{
  int i;
//  for(i = 0 ; i < MAX_CACHE_SIZE ; i++)
//    cache_force_one(&_cache_buffer[i].elem);
}


/* 
 * cache_eviction 
 *
 * DESC | clear element of list by LRU algorithm
 *
 */
static void cache_eviction(void)
{
  int try_count = 2;
  while(try_count){
    struct list_elem* pos = list_rbegin(&cache);
    struct cache_e* temp = list_entry(pos, struct cache_e, elem);

    if(!lock_try_acquire(&temp->cache_lock))
      continue;
    if(temp->flag == B_VALID){
      try_count--;
      lock_release(&temp->cache_lock);
      return;
    }
    else {
      cache_force_one(temp); 
      try_count--;
      lock_release(&temp->cache_lock);
      return;
    }
  }
}
