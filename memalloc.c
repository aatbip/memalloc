// memalloc - a general purpose memory allocator

#include "memalloc.h"
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

typedef struct _th_cache {
  /*fast_bin has pointers to chunks from 16 bytes to 512 bytes incremented by 8 bytes ie 16, 24, 32 ... 512.*/
  // void *fast_bin[63];
  int a;
  struct _th_cache *next;
  struct _th_cache *prev;
} th_cache_t; // thread local storage

typedef struct _memalloc_ctx {
  /*th_cache points at the thread specific data block of the thread that first called memalloc*/
  th_cache_t *th_cache;
  pthread_key_t th_key;
  pthread_once_t once;
  /*Beginning of the heap memory region*/
  void *heap;
  /*Pointer to the top of the used heap*/
  void *top;
  pthread_mutex_t mtx_memalloc_ctx_t;
} memalloc_ctx_t;

static memalloc_ctx_t memalloc_ctx = {.once = PTHREAD_ONCE_INIT, .mtx_memalloc_ctx_t = PTHREAD_MUTEX_INITIALIZER};

void init_once(void) {
  // setup initial heap memory region of 4k
  void *block = sbrk(4096);
  if (block == (void *)-1) {
    perror("sbrk");
  }
  memalloc_ctx.heap = block;
  memalloc_ctx.top = block;

  int c = pthread_key_create(&memalloc_ctx.th_key, NULL);
  if (c != 0) {
    perror("pthread_key_create");
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
    tcache = memalloc_ctx.top;
    tcache->next = NULL;
    memalloc_ctx.top = memalloc_ctx.top + sizeof(th_cache_t);
    pthread_mutex_unlock(&memalloc_ctx.mtx_memalloc_ctx_t);
    int c = pthread_setspecific(memalloc_ctx.th_key, tcache);
    if (c != 0) {
      perror("pthread_setspecific");
    }
  }
  tcache->a = size;
  return tcache;
}

void *func(void *p) {
  th_cache_t *t = memalloc(10);
  printf("from func: %d\n", t->a);
  return NULL;
}

void *func1(void *p) {
  th_cache_t *t = memalloc(12);
  printf("from func1: %d\n", t->a);
  return NULL;
}

int main(void) {
  pthread_t th, th1;
  pthread_create(&th, NULL, func, NULL);
  pthread_create(&th1, NULL, func1, NULL);
  pthread_join(th, NULL);
  pthread_join(th1, NULL);
  return 0;
}
