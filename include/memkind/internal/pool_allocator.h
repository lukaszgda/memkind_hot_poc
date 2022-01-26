#include "stddef.h"
#include "stdint.h"

#include "memkind/internal/slab_allocator.h"

typedef struct PoolAllocator {
    slab_alloc_t *pool[UINT16_MAX];
    slab_alloc_t slabSlabAllocator;
} PoolAllocator;

extern int pool_create(PoolAllocator *pool);
extern void pool_destroy(PoolAllocator *pool);

extern void *pool_malloc(PoolAllocator *pool, size_t size);
extern void *pool_realloc(PoolAllocator *pool, void *ptr, size_t size);
extern void pool_free(void *ptr);
