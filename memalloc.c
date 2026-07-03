// memalloc - a general purpose memory allocator

#include "memalloc.h"
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct _th_cache {
  /*fast_bin has pointers to chunks from 16 bytes to 512 bytes incremented by 8 bytes ie 16, 24, 32 ... 512.*/
  void *fast_bin[63];
} th_cache_t;

typedef struct _memalloc_ctx {
  /*th_cache points at the thread specific data block of the thread that first called memalloc*/
  th_cache_t *th_cache;
  pthread_key_t th_key;
  pthread_once_t once;
  /*Bitmap: from LSB, first bit (init bit)- set if struct is initialized*/
  uint8_t bm;
} memalloc_ctx_t;

static memalloc_ctx_t memalloc_ctx;

void *memalloc(size_t size) {
  // should run once
  if (!(0x01 & memalloc_ctx.bm)) {
    memalloc_ctx.bm |= 0x01; // set init bit
    memalloc_ctx.once = PTHREAD_ONCE_INIT;
  }

  return NULL;
}

int main(void) {
  memalloc(0);
  return 0;
}
