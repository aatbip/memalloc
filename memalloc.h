#ifndef MEMALLOC_H
#define MEMALLOC_H

#include <stddef.h>

void *memalloc(size_t size);
void memfree(void *chunk);

// Tunables (tunable_param macros)-
/*This macro is used to tune the size of block allocated for tcache equivalent to multiple of MEMTUNABLE_NPROC. If not
 * explicitly supplied, memalloc uses `sysconf` to allocate memory in multiple of the available CPU cores.*/
#define MEMTUNABLE_NPROC 2

int memtunable(int tunable_param, int value);

#endif // !MEMALLOC_H
