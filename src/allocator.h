#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stdlib.h>
#include <stdint.h>

#define WORKER_BUMP_SIZE 102400

typedef struct {
  uint64_t offset;
  uint64_t size;
  uint8_t* mem;
} bump;

void *bump_init(bump *b, uint64_t size);
void *bump_alloc(bump *b, uint64_t size, uint64_t alignment);
void bump_reset(bump *b);

#endif
