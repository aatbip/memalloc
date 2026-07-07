// memalloc - a general purpose memory allocator

#include "memalloc.h"
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct _th_cache {
  /*fast_bin has pointers to chunks from 16 bytes to 1024 bytes spaced at 16 bytes ie 16, 32, 48 ... 1024.*/
  void *fast_bin[64];
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

void init_once(void) {
  // setup initial heap memory region of 4k
  void *block = sbrk(4096);
  if (block == (void *)-1) {
    perror("sbrk");
  }
  memalloc_ctx.heap = block;
  memalloc_ctx.top = block;

  int c = pthread_mutex_init(&memalloc_ctx.mtx_memalloc_ctx_t, NULL);
  if (c != 0) {
    perror("pthread_mutex_init");
  }

  c = pthread_key_create(&memalloc_ctx.th_key, NULL);
  if (c != 0) {
    perror("pthread_key_create");
  }
}

static void *fastbin_block_assign(int offset) {
  /*16 byte header field and memory to be returned at 16 byte boundary. The block assigned should be huge enough
   * to allocate 10 times of s.*/
  int s = 16 * (offset + 1) + 16 * 10;
  void *cur_top = memalloc_ctx.top;
  memalloc_ctx.top = cur_top + s;
  return cur_top;
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
    tcache = memalloc_ctx.top;
    if (!memalloc_ctx.recent_th_cache) {
      tcache->next = NULL;
      tcache->prev = NULL;
    } else {
      memalloc_ctx.recent_th_cache->next = tcache;
      tcache->prev = memalloc_ctx.recent_th_cache;
      tcache->next = NULL;
    }
    memalloc_ctx.recent_th_cache = tcache;
    memalloc_ctx.top = memalloc_ctx.top + sizeof(th_cache_t);
    pthread_mutex_unlock(&memalloc_ctx.mtx_memalloc_ctx_t);
    int c = pthread_setspecific(memalloc_ctx.th_key, tcache);
    if (c != 0) {
      perror("pthread_setspecific");
    }
  }
  int offset = size <= 16 ? 0 : ceil((float)(size - 16) / 16);
  void *block = *(tcache->fast_bin + offset);
  if (!block) {
    block = fastbin_block_assign(offset);
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
