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

#define PAGE_SIZE 4096 // 4K

#define MIN_CHUNK_SIZE 16
#define CHUNK_PAD 8 // 8 bytes padding in chunk for alignment to 16 bytes boundary

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

#define GET_FASTBIN_OFFSET(size) (size <= 16 ? 0 : ceil((float)(size - MIN_CHUNK_SIZE) / MIN_CHUNK_SIZE))

typedef struct _el_fastbin {
  /*Points at the starting address of the fastbin block of a specified size.*/
  void *block;
  /*Freelist of the fastbin of the specified size.*/
  void *freelist;
  /*Top of the used block*/
  void *top;
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

/*Increments the program break then updates required members in `memalloc_ctx`*/
static void incr_pgbrk(size_t size) {
  void *block = sbrk(size);
  if (block == (void *)-1) {
    perror("sbrk");
  }
  if (!memalloc_ctx.heap && !memalloc_ctx.top) {
    memalloc_ctx.heap = block;
    memalloc_ctx.top = block;
  }
}

/*Returns a block from the address space of `size` bytes-
 * This function updates the current `top` from memalloc_ctx then returns address
 * of the previous `top`.*/
static void *get_block(size_t size) {
  /*Get a huge heap (4 times PAGE_SIZE) if heap is being incremented the very first time.*/
  if (!memalloc_ctx.heap && !memalloc_ctx.top) {
    incr_pgbrk(PAGE_SIZE * 4);
  }
  /* Increment heap by PAGE_SIZE if the currently unused heap region is not enough for the block `size` request
   * i.e. `sbrk(0)-memalloc_ctx.top <= size`
   * Note: The `incr_pgbr` policy might need to be updated or rethink later on!*/
  if ((char *)sbrk(0) - (char *)memalloc_ctx.top <= size) {
    incr_pgbrk(PAGE_SIZE);
  }
  void *cur_top = memalloc_ctx.top;
  memalloc_ctx.top = cur_top + size;
  return cur_top;
}

/*Fastpath allocation strategy-
 * Allocate in fastbin.*/
static void *fastpath_allocation(th_cache_t *tcache, int size) {
  int offset = GET_FASTBIN_OFFSET(size);
  int fastbin_size = MIN_CHUNK_SIZE * (offset + 1);
  int chunk_size = fastbin_size + CHUNK_HEADER_SIZE;
  el_fastbin_t *fastbin_slot = tcache->fast_bin + offset;
  if (!fastbin_slot->block) {
    int block_size = chunk_size * INITIAL_CHUNK_COUNT;
    fastbin_slot->block = get_block(block_size);
    fastbin_slot->freelist = NULL;
    fastbin_slot->top = fastbin_slot->block;
  }
  if (fastbin_slot->freelist) {
    return fastbin_slot->freelist;
  }
  void *chunk = fastbin_slot->top;
  size_t *p = chunk;
  *p = fastbin_size;
  fastbin_slot->top += chunk_size;
  return chunk + sizeof(size_t) + CHUNK_PAD;
}

/*Function parameter to pass in pthread_once.*/
static void init_once(void) {
  int c = pthread_mutex_init(&memalloc_ctx.mtx_memalloc_ctx_t, NULL);
  if (c != 0) {
    perror("pthread_mutex_init");
  }

  c = pthread_key_create(&memalloc_ctx.th_key, NULL);
  if (c != 0) {
    perror("pthread_key_create");
  }
}

/*Initialize thread specific cache block and required meta data*/
void *init_tcache() {
  int c = pthread_once(&memalloc_ctx.once, init_once);
  if (c != 0) {
    perror("pthread_once");
  }
  th_cache_t *tcache;
  tcache = pthread_getspecific(memalloc_ctx.th_key);
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
  return tcache;
}

void *memalloc(size_t size) {
  th_cache_t *tcache = init_tcache();

  /*Follow fastpath for allocation request of size <=FASTPATH_MAX_LIMIT*/
  if (size <= FASTBIN_MAX_LIMIT) {
    return fastpath_allocation(tcache, size);
  }

  return NULL;
}

void *func(void *p) {
  void *t = memalloc(12);
  printf("func: %d\n", *((char *)t - 16));
  void *s = memalloc(12);
  printf("func a: %d\n", *((char *)s - 16));
  return NULL;
}

void *func1(void *p) {
  void *t = memalloc(12);
  printf("func1: %d\n", *((char *)t - 16));
  void *s = memalloc(12);
  printf("func1 a: %d\n", *((char *)s - 16));
  return NULL;
}

int main(void) {
  int *t = (int *)memalloc(sizeof(int) * 3);
  t[0] = 1;
  t[1] = 2;
  t[2] = 3;
  for (int i = 0; i < 3; i++) {
    printf("%d ", t[i]);
  }
  int *s = (int *)memalloc(sizeof(int) * 3);
  s[0] = 4;
  s[1] = 5;
  s[2] = 6;
  for (int i = 0; i < 3; i++) {
    printf("%d ", s[i]);
  }
  printf("\n");

  pthread_t th, th1;
  pthread_create(&th, NULL, func, NULL);
  pthread_create(&th1, NULL, func1, NULL);
  pthread_join(th, NULL);
  pthread_join(th1, NULL);
  return 0;
}
