#ifndef MEMALLOC_H
#define MEMALLOC_H

#include <stddef.h>

void *memalloc(size_t size);
void memfree(void *chunk);

#endif // !MEMALLOC_H
