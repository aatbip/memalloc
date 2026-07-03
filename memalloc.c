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
} th_cache_t;

typedef struct _memalloc_ctx {
  /*th_cache points at the thread specific data block of the thread that first called memalloc*/
  th_cache_t *th_cache;
  pthread_key_t th_key;
  pthread_once_t once;
  /*Bitmap: from LSB, first bit (init bit)- set if struct is initialized*/
  uint8_t bm;
  void *block;
} memalloc_ctx_t;

static memalloc_ctx_t memalloc_ctx;

void create_key(void) {
  int c = pthread_key_create(&memalloc_ctx.th_key, NULL);
  if (c != 0) {
    perror("pthread_key_create");
  }
}

void *memalloc(size_t size) {
  // should run once
  if (!(0x01 & memalloc_ctx.bm)) {
    memalloc_ctx.bm |= 0x01; // set init bit
    memalloc_ctx.once = PTHREAD_ONCE_INIT;

    // setup initial heap memory region of 4k
    void *block = sbrk(4096);
    if (block == (void *)-1) {
      perror("sbrk");
    }
    memalloc_ctx.block = block;
  }
  int c = pthread_once(&memalloc_ctx.once, create_key);
  if (c != 0) {
    perror("pthread_once");
  }
  th_cache_t *tcache;
  tcache = pthread_getspecific(memalloc_ctx.th_key);
  // allocate
  if (!tcache) {
    tcache = memalloc_ctx.block;
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

int main(void) {
  th_cache_t *t = memalloc(99);
  printf("from main: %d\n", t->a);
  pthread_t th;
  pthread_create(&th, NULL, func, NULL);
  pthread_join(th, NULL);
  return 0;
}
