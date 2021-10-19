#include "bigary.h"
#include "pthread.h"
#include "assert.h"
#include "stdbool.h"
#include "stdint.h"
#include "stddef.h"
#include "string.h"

#include "stdatomic.h"

#define USE_LOCKLESS

// -------- typedefs ----------------------------------------------------------

/// metadata
#ifdef USE_LOCKLESS
typedef _Atomic(struct freelist_node_meta*) freelist_node_thread_safe_meta_ptr;
#else
typedef struct freelist_node_meta* freelist_node_thread_safe_meta_ptr;
#endif

typedef struct freelist_node_meta {
    // pointer required to know how to free and which free lists to put it on
    struct slab_alloc *allocator;
//     size_t size; not stored - all allocations have the same size
    struct freelist_node_meta *next;
} freelist_node_meta_t;

typedef struct glob_free_list {
    freelist_node_thread_safe_meta_ptr freelist;
    pthread_mutex_t mutex;
} glob_free_list_t;

typedef struct slab_alloc {
    // TODO make thread local
    glob_free_list_t globFreelist;
    bigary mappedMemory;
    size_t elementSize;
    // TODO stdatomic would poses issues to c++,
    // a wrapper might be necessary
    atomic_size_t used;
} slab_alloc_t;

// -------- static functions --------------------------------------------------

freelist_node_meta_t *slab_alloc_addr_to_node_meta_(void *addr) {
    assert(addr);
    return (freelist_node_meta_t*)(((uint8_t*)addr)-sizeof(freelist_node_meta_t));
}

void *slab_alloc_node_meta_to_addr_(freelist_node_meta_t *meta) {
    assert(meta);
    return (void*)(((uint8_t*)meta)+sizeof(freelist_node_meta_t));
}

#ifdef USE_LOCKLESS

static void slab_alloc_glob_freelist_push_(void *addr) {
    freelist_node_meta_t *meta = slab_alloc_addr_to_node_meta_(addr);
    slab_alloc_t *alloc = meta->allocator;
    do {
        meta->next = alloc->globFreelist.freelist;
    } while (!atomic_compare_exchange_weak(&alloc->globFreelist.freelist, &meta->next, meta));
}

static void *slab_alloc_glob_freelist_pop_(slab_alloc_t *alloc) {
    freelist_node_meta_t *meta = NULL;
    do {
        meta = alloc->globFreelist.freelist;
        if (!meta)
            break;
    } while (!atomic_compare_exchange_weak(&alloc->globFreelist.freelist, &meta, meta->next));

    return meta ? slab_alloc_node_meta_to_addr_(meta) : NULL;
}

#else

static void slab_alloc_glob_freelist_lock_(slab_alloc_t *alloc) {
    int ret = pthread_mutex_lock(&alloc->globFreelist.mutex);
    assert(ret == 0 && "mutex lock failed!");
}

static void slab_alloc_glob_freelist_unlock_(slab_alloc_t *alloc) {
    int ret = pthread_mutex_unlock(&alloc->globFreelist.mutex);
    assert(ret == 0 && "mutex unlock failed!");
}

static void slab_alloc_glob_freelist_push_(void *addr) {
    freelist_node_meta_t *meta = slab_alloc_addr_to_node_meta_(addr);
    slab_alloc_t *alloc = meta->allocator;
    slab_alloc_glob_freelist_lock_(alloc);
    meta->next = alloc->globFreelist.freelist;
    alloc->globFreelist.freelist = meta;
    slab_alloc_glob_freelist_unlock_(alloc);
}

static void *slab_alloc_glob_freelist_pop_(slab_alloc_t *alloc) {
    slab_alloc_glob_freelist_lock_(alloc);
    freelist_node_meta_t *meta = alloc->globFreelist.freelist;
    if (meta)
        alloc->globFreelist.freelist = alloc->globFreelist.freelist->next;
    slab_alloc_glob_freelist_unlock_(alloc);

    return meta ? slab_alloc_node_meta_to_addr_(meta) : NULL;
}

#endif

static size_t slab_alloc_fetch_increment_used_(slab_alloc_t *alloc) {
    // the value is never atomically decreased, only thing we need is
    return atomic_fetch_add_explicit(&alloc->used, 1u, memory_order_relaxed);
}

static freelist_node_meta_t *slab_alloc_create_meta_(slab_alloc_t *alloc) {
    size_t free_idx = slab_alloc_fetch_increment_used_(alloc);
    size_t meta_offset = alloc->elementSize*free_idx;

    bigary_alloc(&alloc->mappedMemory, (free_idx + 1) * alloc->elementSize);
    // TODO add error handling gracefully instead of die!!!
    // TODO make sure that alignment is correct
    void *ret_ = ((uint8_t*)alloc->mappedMemory.area) + meta_offset;
    freelist_node_meta_t *ret = ret_;
    ret->allocator=alloc;
    ret->next=NULL;

    return ret;
}

// -------- public functions --------------------------------------------------

int slab_alloc_init(slab_alloc_t *alloc, size_t element_size, size_t max_elements) {
//     alloc.globFreelist;
    // TODO remove "die" from bigary_alloc and handle init
    // errors in a gentle way
    alloc->elementSize = sizeof(freelist_node_meta_t)+element_size;
    size_t max_elements_size = max_elements * alloc->elementSize;
    bigary_init(&alloc->mappedMemory, BIGARY_DRAM, max_elements_size);
    alloc->used=0u;

    int ret = pthread_mutex_init(&alloc->globFreelist.mutex, NULL);
    alloc->globFreelist.freelist = NULL;
    if (ret != 0)
        return ret;

    return ret;
}

void slab_alloc_destroy(slab_alloc_t *alloc) {
    int ret = pthread_mutex_destroy(&alloc->globFreelist.mutex);
    bigary_destroy(&alloc->mappedMemory);
    assert(ret == 0 && "mutex destruction failed");
}

void *slab_alloc_malloc(slab_alloc_t *alloc) {
    void *ret = slab_alloc_glob_freelist_pop_(alloc);
    if (!ret) {
        freelist_node_meta_t *meta = slab_alloc_create_meta_(alloc);
        if (meta) // defensive programming
            ret = slab_alloc_node_meta_to_addr_(meta);
    }
    return ret;
}

void slab_alloc_free(void *addr) {
    assert(addr);
    slab_alloc_glob_freelist_push_(addr);
}



#define struct_bar(size) typedef struct bar##size { char boo[(size)]; } bar##size
// TODO add modification of allocated memory
#define test_slab_alloc(size, nof_elements) \
    do { \
        struct_bar(size); \
        slab_alloc_t temp; \
        int ret = slab_alloc_init(&temp, size, nof_elements); \
        assert(ret == 0 && "mutex creation failed!"); \
        slab_alloc_destroy(&temp); \
        ret = slab_alloc_init(&temp, size, nof_elements); \
        assert(ret == 0 && "mutex creation failed!"); \
        bar##size *elements[nof_elements]; \
        for (size_t i=0; i<nof_elements; ++i) { \
            elements[i] = slab_alloc_malloc(&temp); \
            assert(elements[i] && "slab returned NULL!"); \
            memset(elements[i], i, size); \
        } \
        for (int i=0; i<nof_elements; ++i) { \
            for (size_t j=0; j<size; j++) \
                assert(elements[i]->boo[j] == (char)((unsigned)(i))%255); \
        } \
        assert(temp.used == nof_elements); \
        for (int i=0; i<nof_elements; ++i) { \
            slab_alloc_free(elements[i]); \
        } \
        assert(temp.used == nof_elements); \
        for (size_t i=0; i<nof_elements; ++i) { \
            elements[i] = slab_alloc_malloc(&temp); \
            assert(elements[i] && "slab returned NULL!"); \
            memset(elements[i], i+15, size); \
        } \
        for (int i=0; i<nof_elements; ++i) { \
            for (size_t j=0; j<size; j++) \
                assert(elements[i]->boo[j] == (char)((unsigned)(i+15))%255); \
        } \
        assert(temp.used == nof_elements); \
        for (int i=0; i<nof_elements; ++i) { \
            slab_alloc_free(elements[i]); \
        } \
        assert(temp.used == nof_elements); \
        slab_alloc_destroy(&temp); \
    } while (0)

#define struct_bar_align(size) typedef struct bar_align##size { uint64_t boo[(size)]; } bar_align##size
// TODO add modification of allocated memory
#define test_slab_alloc_alignment(size, nof_elements) \
    do { \
        struct_bar_align(size); \
        size_t bar_align_size=sizeof(bar_align##size); \
        slab_alloc_t temp; \
        int ret = slab_alloc_init(&temp, bar_align_size, nof_elements); \
        assert(ret == 0 && "mutex creation failed!"); \
        bar_align##size *elements[nof_elements]; \
        for (size_t i=0; i<nof_elements; ++i) { \
            elements[i] = slab_alloc_malloc(&temp); \
            assert(elements[i] && "slab returned NULL!"); \
            for (size_t j=0; j<size; ++j) \
                elements[i]->boo[j] = i*nof_elements+j; \
        } \
        for (size_t i=0; i<nof_elements; ++i) { \
            for (size_t j=0; j<size; j++) \
                assert(elements[i]->boo[j] == i*nof_elements+j); \
        } \
        assert(temp.used == nof_elements); \
        for (size_t i=0; i<nof_elements; ++i) { \
            slab_alloc_free(elements[i]); \
        } \
        assert(temp.used == nof_elements); \
        for (size_t i=0; i<nof_elements; ++i) { \
            elements[i] = slab_alloc_malloc(&temp); \
            assert(elements[i] && "slab returned NULL!"); \
            for (size_t j=0; j<size; ++j) \
                elements[i]->boo[j] = 7*i*nof_elements+j+5; \
        } \
        for (size_t i=0; i<nof_elements; ++i) { \
            for (size_t j=0; j<size; j++) \
                assert(elements[i]->boo[j] == 7*i*nof_elements+j+5); \
        } \
        assert(temp.used == nof_elements); \
        for (size_t i=0; i<nof_elements; ++i) { \
            slab_alloc_free(elements[i]); \
        } \
        assert(temp.used == nof_elements); \
        slab_alloc_destroy(&temp); \
    } while (0)

static void test_slab_alloc_static3(void) {
    struct_bar(3);
    size_t NOF_ELEMENTS=1024;
    size_t SIZE=3;
    slab_alloc_t temp;
    int ret = slab_alloc_init(&temp, SIZE, NOF_ELEMENTS);
    assert(ret == 0 && "slab alloc init failed!");
    slab_alloc_destroy(&temp);
    ret = slab_alloc_init(&temp, SIZE, NOF_ELEMENTS);
    assert(ret == 0 && "slab alloc init failed!");
    bar3 *elements[NOF_ELEMENTS];
    for (int i=0; i<NOF_ELEMENTS; ++i) {
        elements[i] = slab_alloc_malloc(&temp);
        assert(elements[i] && "slab returned NULL!");
        memset(elements[i], i, SIZE);
    }
    for (int i=0; i<NOF_ELEMENTS; ++i) {
        for (size_t j=0; j<SIZE; j++)
            assert(elements[i]->boo[j] == (char)((unsigned)(i))%255);
    }
    assert(temp.used == NOF_ELEMENTS);
    for (int i=0; i<NOF_ELEMENTS; ++i) {
        slab_alloc_free(elements[i]);
    }
    assert(temp.used == NOF_ELEMENTS);
    for (int i=0; i<NOF_ELEMENTS; ++i) {
        elements[i] = slab_alloc_malloc(&temp);
        assert(elements[i] && "slab returned NULL!");
        memset(elements[i], i+15, SIZE);
    }
    for (int i=0; i<NOF_ELEMENTS; ++i) {
        for (size_t j=0; j<SIZE; j++)
            assert(elements[i]->boo[j] == (char)((unsigned)(i+15))%255);
    }
    assert(temp.used == NOF_ELEMENTS);
    for (int i=0; i<NOF_ELEMENTS; ++i) {
        slab_alloc_free(elements[i]);
    }
    assert(temp.used == NOF_ELEMENTS);
    slab_alloc_destroy(&temp);
}

void test(void) {
    test_slab_alloc_static3();
    test_slab_alloc(1, 1000000);
    test_slab_alloc(2, 1002300);
    test_slab_alloc(4, 798341);
    test_slab_alloc(8, 714962);
    test_slab_alloc(8, 1000000);
    test_slab_alloc(7, 942883);
    test_slab_alloc(17, 71962);
    test_slab_alloc(58, 214662);
}

void test_alignment(void) {
    test_slab_alloc_alignment(1, 100000);
    test_slab_alloc_alignment(1, 213299);
    test_slab_alloc_alignment(2, 912348);
    test_slab_alloc_alignment(4, 821429);
    test_slab_alloc_alignment(8, 814322);
    test_slab_alloc_alignment(7, 291146);
    test_slab_alloc_alignment(7, 291);
}

int main (int argc, char *argv[]) {
    test();
    test_alignment();
}
