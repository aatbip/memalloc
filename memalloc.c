// memalloc - a general purpose memory allocator

#include "memalloc.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <threads.h>
#include <unistd.h>

#define PAGE_SIZE 4096           // 4K
#define FASTBIN_ARENA_SIZE 65536 // 64K

#define MIN_CHUNK_SIZE 16
#define CHUNK_PAD 8 // 8 bytes padding in chunk for alignment to 16 bytes boundary

/*The fastbin block size should be equivalent to fit upto 16 chunks.*/
#define INITIAL_CHUNK_COUNT 16

/* Header (16 bytes) and data block layout:
 *
 * --------------------->chunk
 * size_t size (8 bytes)
 * ---------------------
 * Padding (8 bytes)
 * ---------------------
 * FASTBIN_DATA_BLOCK
 *  */
#define CHUNK_HEADER_SIZE 16
#define FASTBIN_DATA_BLOCK(chunk) ((char *)chunk + sizeof(size_t) + CHUNK_PAD)
#define GET_HEADER_POINTER(chunk) ((char *)chunk - (sizeof(size_t) + CHUNK_PAD))

/*Fast path for allocation request of size <=1024 bytes*/
#define FASTBIN_MAX_LIMIT 1024

#define GET_FASTBIN_OFFSET(size) ((size - 1) / MIN_CHUNK_SIZE)

#define err_exit(msg)                                                                                                  \
  do {                                                                                                                 \
    char buf[64];                                                                                                      \
    strerror_r(errno, buf, 64);                                                                                        \
    const char *m_ = (msg);                                                                                            \
    (void)!write(STDERR_FILENO, m_, strlen(m_));                                                                       \
    (void)!write(STDERR_FILENO, ": ", 2);                                                                              \
    (void)!write(STDERR_FILENO, buf, strlen(buf));                                                                     \
    (void)!write(STDERR_FILENO, "\n", 1);                                                                              \
    exit(EXIT_FAILURE);                                                                                                \
  } while (0)

typedef struct _el_fastbin {
  /*Points at the starting address of the fastbin block of a specified size.*/
  void *block;
  /*Freelist of the fastbin of the specified size.*/
  void *freelist;
  /*Top of the used block*/
  void *top;
} el_fastbin_t;

typedef struct __attribute__((aligned(64))) _th_cache {
  /*fast_bin has pointers to chunks from 16 bytes to 1024 bytes spaced at 16 bytes ie 32, 48 ... 1024.*/
  el_fastbin_t fast_bin[64];
  struct _th_cache *next;
  struct _th_cache *prev;
} th_cache_t; // thread local storage (TLS)

_Thread_local th_cache_t *tcache;

typedef struct _memalloc_ctx {
  pthread_key_t th_key;
  pthread_once_t once;
  /*Pointer to the recently created thread cache block. Used to manage doubly linked list for tcache blocks.*/
  th_cache_t *recent_th_cache;
  /*Pointer to the beginning of the tcache pages block*/
  void *tcache_page;
  /*Pointer to the beginning of the unused tcache pages block*/
  void *tcache_unused_page;

  /*Pointer to the beginning of the 64KB fastbin arena*/
  void *fastbin_arena;
  /*Pointer to the top of the unused 64KB fastbin arena*/
  void *fastbin_arena_top;

  pthread_mutex_t mtx_memalloc_ctx_t;
  /*No. of configured processors in the system*/
  int nproc;
} memalloc_ctx_t;

static memalloc_ctx_t memalloc_ctx = {.once = PTHREAD_ONCE_INIT};

/*Returns a block of `size` bytes from the current 64KB fastbin arena*/
static void *get_block_fastbin_arena(size_t size) {
  pthread_mutex_lock(&memalloc_ctx.mtx_memalloc_ctx_t);
  if (__builtin_expect(
          (char *)memalloc_ctx.fastbin_arena_top + size > (char *)memalloc_ctx.fastbin_arena + FASTBIN_ARENA_SIZE, 0)) {
    /*todo:This is the case when 64KB arena can't handle the request due to insufficient space.*/
  }
  void *block = memalloc_ctx.fastbin_arena_top;
  memalloc_ctx.fastbin_arena_top = (char *)memalloc_ctx.fastbin_arena_top + size;
  pthread_mutex_unlock(&memalloc_ctx.mtx_memalloc_ctx_t);
  return block;
}

/*Fastpath allocation strategy-
 * Allocate in fastbin.*/
static void *fastpath_allocation(th_cache_t *tcache, int size) {
  int offset = GET_FASTBIN_OFFSET(size);
  int block_size = MIN_CHUNK_SIZE * (offset + 1);
  int chunk_size = block_size + CHUNK_HEADER_SIZE;
  int fastbin_size = chunk_size * INITIAL_CHUNK_COUNT;
  el_fastbin_t *fastbin_slot = tcache->fast_bin + offset;

  if (fastbin_slot->freelist) {
    void *cur = fastbin_slot->freelist;
    fastbin_slot->freelist = *(void **)((char *)cur + CHUNK_HEADER_SIZE);
    return FASTBIN_DATA_BLOCK(cur);
  }

  if (!fastbin_slot->block) {
    fastbin_slot->block = get_block_fastbin_arena(fastbin_size);
    fastbin_slot->freelist = NULL;
    fastbin_slot->top = fastbin_slot->block;
  }

  if ((char *)fastbin_slot->block + fastbin_size == fastbin_slot->top) {
    /*This condition is true if allocation request of a `size` bytes from a thread has been more than
     * INITIAL_CHUNK_COUNT times. So, there are already INITIAL_CHUNK_COUNT number of chunks of a specific `size`
     * allocated in a fastbin of a thread. In this case, the allocator first gets the new block using `get_block`.
     * Then it updates (links) the padding bytes of the first chunk of the new block to the `fastbin_slot->block`.
     * Then updates `fastbin_slot->block` to point at the new block.
     *
     * Making use of the padding bytes as a next pointer will save space in the `el_fastbin_t` struct. The reason
     * for using the first chunk for this is to follow spatial locality principle.
     *
     * Todo [improvement]: May be later on we can steal the block from other fastbin_slot of the same tcache or even
     * fastbin_slot of another tcache probably. For ex: If 16 bytes and 32 bytes are allocated in the fast path and then
     * already upto 10 allocation requests are made for 16 bytes size but only a few 32 bytes allocations are made then
     * we can steal chunks from 32 bytes block of the same or different tcache.
     * i.e. first try, steal_block() if not possible then back to get_block()
     */

    void *new_block = get_block_fastbin_arena(fastbin_size);
    /*Padding byte of the new block's first chunk*/
    void **padding_byte = (void *)((char *)new_block + sizeof(size_t));
    *padding_byte = fastbin_slot->block;

    fastbin_slot->block = new_block;
    fastbin_slot->top = new_block;
  }
  void *chunk = fastbin_slot->top;
  size_t *p = chunk;
  *p = chunk_size;
  fastbin_slot->top += chunk_size;
  return FASTBIN_DATA_BLOCK(chunk);
}

/*Runs once a thread returns or cancels. This function can be used to handle the tcache after thread returns and
 * probably the chunks. Possible things to do here:
 *-what happens to tcache when thread dies? will it be tracked to be reused later?
 *-what happens to the fastbin chunks allocated in this tcache? will the unused chunks be reused to given back to the
 OS? how to handle the chunks that are possibly being used "stolen" by other threads (tcaches)?*/
void thread_exit_destr(void *arg) {
  char *msg = "thread_exit_destr()\n";
  if (write(STDOUT_FILENO, msg, strlen(msg)) < strlen(msg)) {
    err_exit("thread_exit_destr: write()");
  };
}

/*Allocate a block for tcache from the tcache page. This tcache block is thread specific (TLS)*/
void *get_tcache_block() {
  if (!memalloc_ctx.tcache_page)
    return NULL;
  void *block = memalloc_ctx.tcache_unused_page;
  memalloc_ctx.tcache_unused_page = memalloc_ctx.tcache_unused_page + sizeof(th_cache_t);
  return block;
}

/*mmap pages to allocate tcache blocks for the threads. In this tcache pages, thread specific tcache blocks are placed
 * and managed as doubly linked list objects.*/
void *tcache_pages_mmap() {
  int s = sizeof(th_cache_t);
  int tcache_pages = (memalloc_ctx.nproc * s + PAGE_SIZE - 1) / PAGE_SIZE; // required vm pages
  void *block =
      mmap(NULL, PAGE_SIZE * tcache_pages, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (block == MAP_FAILED) {
    err_exit("mmap");
  };
  return block;
}

/*Function parameter to pass in pthread_once.*/
static void init_once(void) {
  int c = pthread_mutex_init(&memalloc_ctx.mtx_memalloc_ctx_t, NULL);
  if (c != 0) {
    err_exit("pthread_mutex_init");
  }

  c = pthread_key_create(&memalloc_ctx.th_key, thread_exit_destr);
  if (c != 0) {
    err_exit("pthread_key_create");
  }

  /*If MEMTUNABLE_NPROC is not applied then initialize it to the active processor count.*/
  if (!memalloc_ctx.nproc) {
    c = sysconf(_SC_NPROCESSORS_CONF);
    if (c == -1) {
      err_exit("sysconf");
    }
    memalloc_ctx.nproc = c;
  }

  memalloc_ctx.tcache_page = tcache_pages_mmap();
  memalloc_ctx.tcache_unused_page = memalloc_ctx.tcache_page;
}

/*Initialize thread specific cache block and required meta data*/
void *init_tcache() {
  pthread_once(&memalloc_ctx.once, init_once);
  if (!tcache) {
    tcache = get_tcache_block();
    pthread_mutex_lock(&memalloc_ctx.mtx_memalloc_ctx_t);
    if (!memalloc_ctx.recent_th_cache) {
      tcache->next = NULL;
      tcache->prev = NULL;
    } else {
      memalloc_ctx.recent_th_cache->next = tcache;
      tcache->prev = memalloc_ctx.recent_th_cache;
      tcache->next = NULL;
    }
    memalloc_ctx.recent_th_cache = tcache;

    /*Mmap 64KB arena.
     * Todo: handle the case when this arena is full. Where to do it? */
    if (!memalloc_ctx.fastbin_arena) {
      void *block = mmap(NULL, FASTBIN_ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (block == MAP_FAILED) {
        err_exit("mmap");
      };
      memalloc_ctx.fastbin_arena = block;
      memalloc_ctx.fastbin_arena_top = block;
    }

    pthread_mutex_unlock(&memalloc_ctx.mtx_memalloc_ctx_t);
    int c = pthread_setspecific(memalloc_ctx.th_key, tcache);
    if (c != 0) {
      err_exit("pthread_setspecific");
    }
  }
  return tcache;
}

void fastbin_free(void *header, size_t chunk_size) {
  th_cache_t *tcache = init_tcache();
  int offset = GET_FASTBIN_OFFSET(chunk_size - CHUNK_HEADER_SIZE);
  el_fastbin_t *fastbin_t = tcache->fast_bin + offset;
  void *cur = fastbin_t->freelist;
  fastbin_t->freelist = header;
  *(void **)((char *)header + CHUNK_HEADER_SIZE) = cur;
}

int memtunable(int tunable_param, int value) {
  if (__builtin_expect(tunable_param == MEMTUNABLE_NPROC, 1)) {
    memalloc_ctx.nproc = value;
    return 0;
  }
  return -1;
}

void memfree(void *chunk) {
  void *header = GET_HEADER_POINTER(chunk);
  size_t chunk_size = *(size_t *)header;
  if (chunk_size <= FASTBIN_MAX_LIMIT) {
    fastbin_free(header, chunk_size);
  }
}

void *memalloc(size_t size) {
  th_cache_t *tcache = init_tcache();

  /*Follow fastpath for allocation request of size <=FASTPATH_MAX_LIMIT*/
  if (size <= FASTBIN_MAX_LIMIT) {
    return fastpath_allocation(tcache, size);
  }

  return NULL;
}

int main(void) {
  memalloc(sizeof(int));

  return 0;
}
