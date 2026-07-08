// memalloc - a general purpose memory allocator

#include "memalloc.h"
#include <math.h>
#include <pthread.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MIN_CHUNK_SIZE 16

/*The fastbin block size should be equivalent to fit upto 10 chunks.*/
#define INITIAL_CHUNK_COUNT 10

/* Header layout (16 bytes):
 *
 * ---------------------
 * size_t size (8 bytes)
 * ---------------------
 * Padding (8 bytes)
 * ---------------------*/
#define CHUNK_HEADER_SIZE 16

/*Fast path for allocation request of size <=1024 bytes*/
#define FASTBIN_MAX_LIMIT 1024

#define GET_FASTBIN_OFFSET(size) (size <= 16 ? 0 : ceil((float)(size - MIN_CHUNK_SIZE) / MIN_CHUNK_SIZE));

typedef struct _el_fastbin {
  /*Points at the starting address of the fastbin block of a specified size.*/
  void *block;
  /*Freelist of the fastbin of the specified size.*/
  void *freelist;
} el_fastbin_t;

typedef struct _th_cache {
  /*fast_bin has pointers to chunks from 16 bytes to 1024 bytes spaced at 16 bytes ie 16, 32, 48 ... 1024.*/
  el_fastbin_t fast_bin[64];
  struct _th_cache *next;
  struct _th_cache *prev;
} th_cache_t; // thread local storage

typedef struct _memalloc_ctx {
  pthread_key_t th_key;
  pthread_once_t once;
  /*Beginning of the heap memory region*/
  void *heap;
  /*Pointer to the top of the used heap*/
  void *top;
  /*Pointer to the recently created thread cache block*/
  th_cache_t *recent_th_cache;
  pthread_mutex_t mtx_memalloc_ctx_t;
} memalloc_ctx_t;

static memalloc_ctx_t memalloc_ctx = {.once = PTHREAD_ONCE_INIT};

/*Increments the program break. For now increments equivalent to page size (4k). Should
 * make more flexible later on!*/
void incr_pgbrk() {
  void *block = sbrk(4096);
  if (block == (void *)-1) {
    perror("sbrk");
  }
  memalloc_ctx.heap = block;
  memalloc_ctx.top = block;
}

void init_once(void) {
  incr_pgbrk();

  int c = pthread_mutex_init(&memalloc_ctx.mtx_memalloc_ctx_t, NULL);
  if (c != 0) {
    perror("pthread_mutex_init");
  }

  c = pthread_key_create(&memalloc_ctx.th_key, NULL);
  if (c != 0) {
    perror("pthread_key_create");
  }
}

/*Returns a block from the address space of `size` bytes-
 * This function updates the current `top` from memalloc_ctx then returns address
 * of the previous `top`.*/
static void *get_block(size_t size) {
  void *cur_top = memalloc_ctx.top;
  memalloc_ctx.top = cur_top + size;
  return cur_top;
}

/*Fastpath allocation strategy-
 * Allocate in fastbin.*/
static void fastpath_allocation(th_cache_t *tcache, int size) {
  int offset = GET_FASTBIN_OFFSET(size);
  el_fastbin_t *fastbin_slot = tcache->fast_bin + offset;
  if (!fastbin_slot->block) {
    int block_size = MIN_CHUNK_SIZE * (offset + 1) + CHUNK_HEADER_SIZE * INITIAL_CHUNK_COUNT;
    fastbin_slot->block = get_block(offset);
    fastbin_slot->freelist = NULL;
  }
}

void *memalloc(size_t size) {
  // should run once
  int c = pthread_once(&memalloc_ctx.once, init_once);
  if (c != 0) {
    perror("pthread_once");
  }
  th_cache_t *tcache;
  tcache = pthread_getspecific(memalloc_ctx.th_key);
  // allocate
  if (!tcache) {
    pthread_mutex_lock(&memalloc_ctx.mtx_memalloc_ctx_t);
    tcache = get_block(sizeof(th_cache_t));
    if (!memalloc_ctx.recent_th_cache) {
      tcache->next = NULL;
      tcache->prev = NULL;
    } else {
      memalloc_ctx.recent_th_cache->next = tcache;
      tcache->prev = memalloc_ctx.recent_th_cache;
      tcache->next = NULL;
    }
    memalloc_ctx.recent_th_cache = tcache;
    pthread_mutex_unlock(&memalloc_ctx.mtx_memalloc_ctx_t);
    int c = pthread_setspecific(memalloc_ctx.th_key, tcache);
    if (c != 0) {
      perror("pthread_setspecific");
    }
  }

  /*Follow fastpath for allocation request of size <=FASTPATH_MAX_LIMIT*/
  if (size <= FASTBIN_MAX_LIMIT) {
    fastpath_allocation(tcache, size);
  }

  return tcache;
}

void *func(void *p) {
  th_cache_t *t = memalloc(32);
  return NULL;
}

void *func1(void *p) {
  th_cache_t *t = memalloc(12);
  return NULL;
}

int main(void) {
  pthread_t th;
  pthread_create(&th, NULL, func, NULL);
  // pthread_create(&th1, NULL, func1, NULL);
  pthread_join(th, NULL);
  // pthread_join(th1, NULL);
  return 0;
}
