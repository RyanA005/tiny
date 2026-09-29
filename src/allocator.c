#include "allocator.h"

void bump_bind(bump *b, void *mem, uint64_t size) {
    b->offset = 0;
    b->size = size;
    b->mem = (uint8_t *)mem;
}

void *bump_alloc(bump *b, uint64_t size, uint64_t alignment) {
    if (!b->mem || alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return 0;
    }

    uintptr_t base = (uintptr_t)b->mem;
    uintptr_t current = base + (uintptr_t)b->offset;
    uintptr_t aligned = (current + (uintptr_t)alignment - 1) & ~((uintptr_t)alignment - 1);

    if (aligned < current) {
        return 0;
    }

    uint64_t offset = (uint64_t)(aligned - base);
    if (offset > b->size) {
        return 0;
    }
    if (size > b->size - offset) {
        return 0;
    }

    b->offset = offset + size;
    return (void *)aligned;
}

void bump_reset(bump *b) {
    b->offset = 0;
}
