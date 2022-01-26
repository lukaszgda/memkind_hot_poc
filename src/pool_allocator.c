#include "string.h"

#include "memkind/internal/pool_allocator.h"
#include "memkind/internal/bthash.h"


int pool_create(PoolAllocator *pool){
    int ret = slab_alloc_init(&pool->slabSlabAllocator, sizeof(slab_alloc_t), UINT16_MAX);
    if (ret == 0)
        (void)memset(pool->pool, 0, sizeof(pool->pool));

    return ret;
}

void pool_destroy(PoolAllocator *pool) {
    for (size_t i=0; i<UINT16_MAX; ++i)
        slab_alloc_free(pool->pool[i]);
    slab_alloc_destroy(&pool->slabSlabAllocator);
}

static uint16_t calculate_hash(size_t size_rank); // TODO implement - here, or elsewhere
static size_t calculate_size_rank(size_t size); // TODO implement - here, or elsewhere
static size_t size_rank_to_size(size_t size_rank);

void *pool_malloc(PoolAllocator *pool, size_t size) {
    if (size == 0)
        return NULL;
    size_t size_rank = calculate_size_rank(size);
    uint16_t hash = calculate_hash(size_rank);
    slab_alloc_t *slab = &pool->pool[hash];
    if (!slab) {
        // TODO initialize the slab in a lockless way
        slab = slab_alloc_malloc(&pool->slabSlabAllocator);
        size_t slab_size = size_rank_to_size(size_rank);
        int ret = slab_alloc_init(slab, slab_size, 0);
        if (ret != 0)
            return NULL;
        // TODO atomic compare exchange WARNING HACK NOT THREAD SAFE NOW !!!!
        pool->pool[hash] = slab;
    }

    return slab_alloc_malloc(slab);
}

void *pool_realloc(PoolAllocator *pool, void *ptr, size_t size) {
    slab_alloc_free(ptr);
    return pool_malloc(pool, size);
}

void pool_free(void *ptr) {
    slab_alloc_free(ptr);
}
